//
// Copyright 2019 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/imaging/garch/glApi.h"

#include "pxr/imaging/hdx/pickTask.h"

#include "pxr/imaging/hdx/debugCodes.h"
#include "pxr/imaging/hdx/package.h"
#include "pxr/imaging/hdx/renderSetupTask.h"
#include "pxr/imaging/hdx/tokens.h"

#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/types.h"

#include "pxr/imaging/hdSt/renderBuffer.h"
#include "pxr/imaging/hdSt/renderDelegate.h"
#include "pxr/imaging/hdSt/renderPassShader.h"
#include "pxr/imaging/hdSt/renderPassState.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hdSt/textureUtils.h"

#include "pxr/imaging/hgi/graphicsCmdsDesc.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/hgiGL/graphicsCmds.h"
#include "pxr/imaging/glf/diagnostic.h"
#include "pxr/imaging/glf/glContext.h"

#include <boost/functional/hash.hpp>

#include <iostream>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdxPickTokens, HDX_PICK_TOKENS);

static HdRenderPassStateSharedPtr
_InitIdRenderPassState(HdRenderIndex *index)
{
    HdRenderPassStateSharedPtr rps =
        index->GetRenderDelegate()->CreateRenderPassState();

    if (HdStRenderPassState* extendedState =
            dynamic_cast<HdStRenderPassState*>(
                rps.get())) {
        extendedState->SetRenderPassShader(
            std::make_shared<HdStRenderPassShader>(
                HdxPackageRenderPassPickingShader()));
    }

    return rps;
}

static bool
_IsStormRenderer(HdRenderDelegate *renderDelegate)
{
    if(!dynamic_cast<HdStRenderDelegate*>(renderDelegate)) {
        return false;
    }

    return true;
}

// Generated renderbuffers
static TfTokenVector _aovOutputs {
    HdAovTokens->primId,
    HdAovTokens->instanceId,
    HdAovTokens->elementId,
    HdAovTokens->edgeId,
    HdAovTokens->pointId,
    HdAovTokens->Neye,
    HdAovTokens->depthStencil
};
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (widgetDepthStencil)
);

static SdfPath 
_GetAovPath(TfToken const& aovName)
{
    std::string identifier = std::string("aov_pickTask_") +
        TfMakeValidIdentifier(aovName.GetString());
    return SdfPath(identifier);
}

// -------------------------------------------------------------------------- //
// HdxPickTask
// -------------------------------------------------------------------------- //
HdxPickTask::HdxPickTask(HdSceneDelegate* delegate, SdfPath const& id)
    : HdTask(id)
    , _allRenderTags()
    , _nonWidgetRenderTags()
    , _index(nullptr)
    , _hgi(nullptr)
    , _pickableDepthIndex(0)
{
}

HdxPickTask::~HdxPickTask()
{
    _CleanupAovBindings();
}

void
HdxPickTask::_InitIfNeeded()
{
    if (_pickableAovBuffers.empty()) {
        _CreateAovBindings();
    }

    for (HdRenderPassAovBinding const & aovBinding : _pickableAovBindings) {
        _ResizeOrCreateBufferForAOV(aovBinding);
    }
    for (HdRenderPassAovBinding const & aovBinding : _widgetAovBindings) {
        _ResizeOrCreateBufferForAOV(aovBinding);
    }

    if (_pickableRenderPass == nullptr || _occluderRenderPass == nullptr ||
        _widgetRenderPass == nullptr) {
        // The collection created below is just for satisfying the HdRenderPass
        // constructor. The collections for the render passes are set in Query.
        HdRprimCollection col(HdTokens->geometry,
            HdReprSelector(HdReprTokens->hull));
        _pickableRenderPass =
            _index->GetRenderDelegate()->CreateRenderPass(&*_index, col);
        _occluderRenderPass =
            _index->GetRenderDelegate()->CreateRenderPass(&*_index, col);
        _widgetRenderPass = 
            _index->GetRenderDelegate()->CreateRenderPass(&*_index, col);

        // initialize renderPassStates with ID render shader
        _pickableRenderPassState = _InitIdRenderPassState(_index);
        _occluderRenderPassState = _InitIdRenderPassState(_index);
        _widgetRenderPassState = _InitIdRenderPassState(_index);
        // Turn off color writes for the occluders, wherein we want to only
        // condition the depth buffer and not write out any IDs.
        // XXX: This is a hacky alternative to using a different shader mixin
        // to accomplish the same thing.
        _occluderRenderPassState->SetColorMaskUseDefault(false);
        _occluderRenderPassState->SetColorMasks(
            {HdRenderPassState::ColorMaskNone});
    }
}

void
HdxPickTask::_CreateAovBindings()
{
    HdStResourceRegistrySharedPtr hdStResourceRegistry =
        std::static_pointer_cast<HdStResourceRegistry>(
            _index->GetResourceRegistry());

    HdRenderDelegate *renderDelegate = _index->GetRenderDelegate();

    GfVec3i dimensions(_contextParams.resolution[0],
                       _contextParams.resolution[1], 1);

    // Add the new renderbuffers.
    for (size_t i = 0; i < _aovOutputs.size(); ++i) {
        TfToken const & aovOutput = _aovOutputs[i];
        SdfPath const aovId = _GetAovPath(aovOutput);

        _pickableAovBuffers.push_back(
            std::make_unique<HdStRenderBuffer>(
                hdStResourceRegistry.get(), aovId));

        HdAovDescriptor aovDesc = renderDelegate->
            GetDefaultAovDescriptor(aovOutput);

        // Convert to a binding.
        HdRenderPassAovBinding binding;
        binding.aovName = aovOutput;
        binding.renderBufferId = aovId;
        binding.aovSettings = aovDesc.aovSettings;
        binding.renderBuffer = _pickableAovBuffers.back().get();
        // Clear all color channels to 1, so when cast as int, an unwritten
        // pixel is encoded as -1.
        binding.clearValue = VtValue(GfVec4f(1));

        _pickableAovBindings.push_back(binding);

        if (HdAovHasDepthStencilSemantic(aovOutput)) {
            _pickableDepthIndex = i;
            _occluderAovBinding = binding;
        }
    }

    // Set up widget render pass' depth binding, a fresh empty depthStencil
    // buffer, so that inter-widget occlusion is correct while widgets all draw
    // in front of any previously-drawn items.  While writing to other AOVs,
    // don't clear them at all, so that previously-drawn items are retained.
    {
        _widgetDepthStencilBuffer = std::make_unique<HdStRenderBuffer>(
            hdStResourceRegistry.get(),
            _GetAovPath(_tokens->widgetDepthStencil));

        HdAovDescriptor depthDesc = renderDelegate->GetDefaultAovDescriptor(
            HdAovTokens->depth);
        
        _widgetAovBindings = _pickableAovBindings;
        for (auto& binding : _widgetAovBindings) {
            binding.clearValue = VtValue();
        }

        HdRenderPassAovBinding widgetDepthBinding;
        widgetDepthBinding.aovName = _tokens->widgetDepthStencil;
        widgetDepthBinding.renderBufferId = _GetAovPath(
            _tokens->widgetDepthStencil);
        widgetDepthBinding.aovSettings = depthDesc.aovSettings;
        widgetDepthBinding.renderBuffer = _widgetDepthStencilBuffer.get();
        widgetDepthBinding.clearValue = VtValue(GfVec4f(1));
        _widgetAovBindings.back() = widgetDepthBinding;
    }
}

void
HdxPickTask::_CleanupAovBindings()
{
    if (_index) {
        HdRenderParam * renderParam =
                                _index->GetRenderDelegate()->GetRenderParam();
        for (auto const & aovBuffer : _pickableAovBuffers) {
            aovBuffer->Finalize(renderParam);
        }
        _widgetDepthStencilBuffer->Finalize(renderParam);
    }
    _pickableAovBuffers.clear();
    _pickableAovBindings.clear();
}

void
HdxPickTask::_ResizeOrCreateBufferForAOV(
    const HdRenderPassAovBinding& aovBinding)

{
    HdRenderDelegate * renderDelegate = _index->GetRenderDelegate();

    GfVec3i dimensions(_contextParams.resolution[0],
                       _contextParams.resolution[1], 1);

    VtValue existingResource = aovBinding.renderBuffer->GetResource(false);

    if (existingResource.IsHolding<HgiTextureHandle>()) {
        int32_t width = aovBinding.renderBuffer->GetWidth();
        int32_t height = aovBinding.renderBuffer->GetHeight();
        if (width == dimensions[0] && height == dimensions[1]) {
            return;
        }
    }

    // If the resolution has changed then reallocate the
    // renderBuffer and  texture.
    HdAovDescriptor aovDesc = renderDelegate->
        GetDefaultAovDescriptor(aovBinding.aovName);

    aovBinding.renderBuffer->Allocate(dimensions,
                                      aovDesc.format,
                                      false);

    VtValue newResource = aovBinding.renderBuffer->GetResource(false);

    if (!newResource.IsHolding<HgiTextureHandle>()) {
        TF_CODING_ERROR("No texture on render buffer for AOV "
                        "%s", aovBinding.aovName.GetText());
    }
}

void
HdxPickTask::_ConditionStencilWithGLCallback(
    HdxPickTaskContextParams::DepthMaskCallback maskCallback,
    HdRenderBuffer const * depthStencilBuffer)
{
    VtValue const resource = depthStencilBuffer->GetResource(false);
    HgiTextureHandle depthTexture = resource.UncheckedGet<HgiTextureHandle>();

    HgiAttachmentDesc attachmentDesc;
    attachmentDesc.format = depthTexture->GetDescriptor().format;
    attachmentDesc.usage = depthTexture->GetDescriptor().usage;
    attachmentDesc.loadOp = HgiAttachmentLoadOpClear;
    attachmentDesc.storeOp = HgiAttachmentStoreOpStore;
    attachmentDesc.clearValue = GfVec4f(0);

    HgiGraphicsCmdsDesc desc;
    desc.depthAttachmentDesc = std::move(attachmentDesc);
    desc.depthTexture = depthTexture;

    HgiGraphicsCmdsUniquePtr gfxCmds = _hgi->CreateGraphicsCmds(desc);
    gfxCmds->PushDebugGroup("PickTask Condition Stencil Buffer");

    GfVec2i dimensions = _contextParams.resolution;
    GfVec4i viewport(0, 0, dimensions[0], dimensions[1]);
    gfxCmds->SetViewport(viewport);

    HgiGLGraphicsCmds* glGfxCmds =
        dynamic_cast<HgiGLGraphicsCmds*>(gfxCmds.get());

    auto executeMaskCallback = [maskCallback] {
        // Setup stencil state and prevent writes to color buffer.
        // We don't use the pickable/unpickable render pass state below, since
        // the callback uses immediate mode GL, and doesn't conform to Hydra's
        // command buffer based execution philosophy.
        {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_FALSE);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 1, 1);
            glStencilOp(GL_KEEP,     // stencil failed
                        GL_KEEP,     // stencil passed, depth failed
                        GL_REPLACE); // stencil passed, depth passed
        }
        
        //
        // Condition the stencil buffer.
        //
        maskCallback();

        // We expect any GL state changes are restored.
        {
            // Clear depth in case the maskCallback pollutes the depth buffer.
            glDepthMask(GL_TRUE);
            glClearDepth(1.0f);
            glClear(GL_DEPTH_BUFFER_BIT);
            // Restore color outputs & setup state for rendering
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_CULL_FACE);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glFrontFace(GL_CCW);
        }
    };

    glGfxCmds->InsertFunctionOp(executeMaskCallback);

    gfxCmds->PopDebugGroup();
    _hgi->SubmitCmds(gfxCmds.get());
}

bool
HdxPickTask::_UseOcclusionPass() const
{
    return _contextParams.doUnpickablesOcclude &&
           !_contextParams.collection.GetExcludePaths().empty();
}

bool
HdxPickTask::_UseWidgetPass() const
{
    return _allRenderTags != _nonWidgetRenderTags;
}

template<typename T>
T const *
HdxPickTask::_ReadAovBuffer(TfToken const & aovName,
                            std::vector<uint8_t> * buffer) const
{
    HdRenderBuffer const * renderBuffer = _FindAovBuffer(aovName);

    VtValue aov = renderBuffer->GetResource(false);
    if (aov.IsHolding<HgiTextureHandle>()) {
        HgiTextureHandle texture = aov.Get<HgiTextureHandle>();

        if (texture) {
            HdStTextureUtils::HgiTextureReadback(_hgi, texture, buffer);
        }
    }

    return reinterpret_cast<T const *>(buffer->data());
}

HdRenderBuffer const *
HdxPickTask::_FindAovBuffer(TfToken const & aovName) const
{
    HdRenderPassAovBindingVector::const_iterator bindingIt =
        std::find_if(_pickableAovBindings.begin(), _pickableAovBindings.end(),
            [&aovName](HdRenderPassAovBinding const & binding) {
                return binding.aovName == aovName;
            });

    if (!TF_VERIFY(bindingIt != _pickableAovBindings.end())) {
        return nullptr;
    }

    return bindingIt->renderBuffer;
}

void
HdxPickTask::Sync(HdSceneDelegate* delegate,
                  HdTaskContext* ctx,
                  HdDirtyBits* dirtyBits)
{
    GLF_GROUP_FUNCTION();

    if (!_IsStormRenderer( delegate->GetRenderIndex().GetRenderDelegate() )) {
        return;
    }

    if (!_hgi) {
        _hgi = HdTask::_GetDriver<Hgi*>(ctx, HgiTokens->renderDriver);
    }

    // Gather params from the scene and the task context.
    if ((*dirtyBits) & HdChangeTracker::DirtyParams) {
        _GetTaskParams(delegate, &_params);
    }

    if ((*dirtyBits) & HdChangeTracker::DirtyRenderTags) {
        _allRenderTags = _GetTaskRenderTags(delegate);
        // Split the supplied render tags into the "widget" tag if any,
        // and the remaining tags.  Later we render these groups in separate
        // passes.
        _nonWidgetRenderTags.clear();
        _nonWidgetRenderTags.reserve(_allRenderTags.size());
        for (const TfToken& tag : _allRenderTags) {
            if (tag != HdxRenderTagTokens->widget) {
                _nonWidgetRenderTags.push_back(tag);
            }
        }
    }

    _GetTaskContextData(ctx, HdxPickTokens->pickParams, &_contextParams);

    // Store the render index so we can map ids to paths in Execute()...
    _index = &(delegate->GetRenderIndex());

    // Make sure we're in a sane GL state before attempting anything.
    GlfGLContextSharedPtr context = GlfGLContext::GetCurrentGLContext();
    if (!TF_VERIFY(context)) {
        TF_RUNTIME_ERROR("Invalid GL context");
        return;
    }

    _InitIfNeeded();

    if (!TF_VERIFY(_pickableRenderPass) || 
        !TF_VERIFY(_occluderRenderPass)) {
        return;
    }
    
    HdRenderPassStateSharedPtr states[] = {_pickableRenderPassState,
                                           _occluderRenderPassState,
                                           _widgetRenderPassState};

    // Are we using stencil conditioning?
    bool needStencilConditioning =
        (_contextParams.depthMaskCallback != nullptr);

    // Calculate the viewport
    GfVec4i viewport(0, 0,
                     _contextParams.resolution[0],
                     _contextParams.resolution[1]);

    // Update the renderpass states.
    for (auto& state : states) {
        if (needStencilConditioning) {
            state->SetStencilEnabled(true);
            state->SetStencil(HdCmpFuncLess,
                            /*ref=*/0,
                            /*mask=*/1,
                            /*sFail*/HdStencilOpKeep,
                            /*sPassZFail*/HdStencilOpKeep,
                            /*sPassZPass*/HdStencilOpKeep);
        } else {
            state->SetStencilEnabled(false);
        }

        state->SetEnableDepthTest(true);
        state->SetEnableDepthMask(true);
        state->SetDepthFunc(HdCmpFuncLEqual);

        // Make sure translucent pixels can be picked by not discarding them
        state->SetAlphaThreshold(0.0f);
        state->SetAlphaToCoverageEnabled(false);
        state->SetBlendEnabled(false);
        state->SetCullStyle(_params.cullStyle);
        state->SetLightingEnabled(false);

        // If scene materials are disabled in this environment then
        // let's setup the override shader
        if (HdStRenderPassState* extState =
                dynamic_cast<HdStRenderPassState*>(state.get())) {
            extState->SetCameraFramingState(
                _contextParams.viewMatrix, 
                _contextParams.projectionMatrix,
                viewport,
                _contextParams.clipPlanes);
            extState->SetUseSceneMaterials(_params.enableSceneMaterials);
        }
    }

    _pickableRenderPassState->SetAovBindings(_pickableAovBindings);
    if (_UseOcclusionPass()) {
        _occluderRenderPassState->SetAovBindings({_occluderAovBinding});
    }
    if (_UseWidgetPass()) {
        _widgetRenderPassState->SetAovBindings(_widgetAovBindings);
    }

    // Update the collections
    //
    // The picking operation is composed of one or more conceptual passes:
    // (i) [optional] depth-only pass for "unpickable" prims: This ensures 
    // that occlusion stemming for unpickable prims is honored during 
    // picking.
    //
    // (ii) [mandatory] id render for "pickable" prims: This writes out the
    // various id's for prims that pass the depth test.
    //
    // (iii) [optional] id render for "widget" prims.  This pass, along with
    // bound color and depth input AOVs, allows widget materials the choice of 
    // drawing always-on-top, blending to show through occluders, or being
    // occluded as normal, depending on their shader behavior.  Note this 
    // drawing scheme leaves widgets out of the shared depth buffer for 
    // simplicity.
    if (_UseOcclusionPass()) {
        // Pass (i) from above
        HdRprimCollection occluderCol =
            _contextParams.collection.CreateInverseCollection();
        _occluderRenderPass->SetRprimCollection(occluderCol);
    }

    // Pass (ii) from above
    _pickableRenderPass->SetRprimCollection(_contextParams.collection);

    // Pass (iii) from above
    if (_UseWidgetPass()) {
        _widgetRenderPass->SetRprimCollection(_contextParams.collection);
    }

    if (_UseOcclusionPass()) {
        _occluderRenderPass->Sync();
    }
    _pickableRenderPass->Sync();
    if (_UseWidgetPass()) {
        _widgetRenderPass->Sync();
    }

    *dirtyBits = HdChangeTracker::Clean;
}

void
HdxPickTask::Prepare(HdTaskContext* ctx,
                     HdRenderIndex* renderIndex)
{
    if (_UseOcclusionPass()) {
        _occluderRenderPassState->Prepare(renderIndex->GetResourceRegistry());
    }
    _pickableRenderPassState->Prepare(renderIndex->GetResourceRegistry());
    if (_UseWidgetPass()) {
        _widgetRenderPassState->Prepare(renderIndex->GetResourceRegistry());
    }
}

void
HdxPickTask::Execute(HdTaskContext* ctx)
{
    GLF_GROUP_FUNCTION();

    GfVec2i dimensions = _contextParams.resolution;
    GfVec4i viewport(0, 0, dimensions[0], dimensions[1]);

    GLuint restoreDrawFB = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, (GLint*)&restoreDrawFB);

    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    // Are we using stencil conditioning?
    bool needStencilConditioning =
        (_contextParams.depthMaskCallback != nullptr);

    if (needStencilConditioning) {
        _ConditionStencilWithGLCallback(_contextParams.depthMaskCallback,
            _pickableAovBindings[_pickableDepthIndex].renderBuffer);
        _ConditionStencilWithGLCallback(_contextParams.depthMaskCallback,
            _widgetDepthStencilBuffer.get());
    }

    //
    // Enable conservative rasterization, if available.
    //
    bool convRstr = GARCH_GLAPI_HAS(NV_conservative_raster);
    if (convRstr) {
        glEnable(GL_CONSERVATIVE_RASTERIZATION_NV);
    }

    if (_UseOcclusionPass()) {
        _occluderRenderPass->Execute(_occluderRenderPassState,
                                     _nonWidgetRenderTags);
        // Prevent the depth from being cleared so that occluders are retained.
        _pickableAovBindings[_pickableDepthIndex].clearValue = VtValue();
    }
    else if (needStencilConditioning) {
        // Prevent depthStencil from being cleared so that stencil is retained.
        _pickableAovBindings[_pickableDepthIndex].clearValue = VtValue();
    }
    else {
        // If there was no occlusion pass and we didn't condition the
        // depthStencil buffer then clear the depth.
        _pickableAovBindings[_pickableDepthIndex].clearValue =
            VtValue(GfVec4f(1.0f));
    }

    // Push the changes to the clearValue into the renderPassState.
    _pickableRenderPassState->SetAovBindings(_pickableAovBindings);
    _pickableRenderPass->Execute(_pickableRenderPassState,
                                 _nonWidgetRenderTags);

    if (_UseWidgetPass()) {
        if (needStencilConditioning) {
            // Prevent widget depthStencil from being cleared so that stencil is
            // retained.
            _widgetAovBindings.back().clearValue = VtValue();
        } else {
            _widgetAovBindings.back().clearValue = VtValue(GfVec4f(1.0f));
        }
        _widgetRenderPassState->SetAovBindings(_widgetAovBindings);
        _widgetRenderPass->Execute(_widgetRenderPassState,
            {HdxRenderTagTokens->widget});
    }

    glDisable(GL_STENCIL_TEST);

    if (convRstr) {
        glDisable(GL_CONSERVATIVE_RASTERIZATION_NV);
    }

    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, restoreDrawFB);

    // Capture the result buffers and cast to the appropriate types.
    std::vector<uint8_t> primIds;
    int const * primIdPtr =
        _ReadAovBuffer<int>(HdAovTokens->primId, &primIds);

    std::vector<uint8_t> instanceIds;
    int const * instanceIdPtr =
        _ReadAovBuffer<int>(HdAovTokens->instanceId, &instanceIds);

    std::vector<uint8_t> elementIds;
    int const * elementIdPtr =
        _ReadAovBuffer<int>(HdAovTokens->elementId, &elementIds);

    std::vector<uint8_t> edgeIds;
    int const * edgeIdPtr =
        _ReadAovBuffer<int>(HdAovTokens->edgeId, &edgeIds);

    std::vector<uint8_t> pointIds;
    int const * pointIdPtr =
        _ReadAovBuffer<int>(HdAovTokens->pointId, &pointIds);

    std::vector<uint8_t> neyes;
    int const * neyePtr =
        _ReadAovBuffer<int>(HdAovTokens->Neye, &neyes);

    std::vector<uint8_t> depths;
    float const * depthPtr =
        _ReadAovBuffer<float>(HdAovTokens->depthStencil, &depths);

    // For un-projection, get the current depth range.
    GLfloat p[2];
    glGetFloatv(GL_DEPTH_RANGE, &p[0]);
    GfVec2f depthRange(p[0], p[1]);

    HdxPickResult result(
            primIdPtr, instanceIdPtr, elementIdPtr,
            edgeIdPtr, pointIdPtr, neyePtr, depthPtr,
            _index, _contextParams.pickTarget, _contextParams.viewMatrix,
            _contextParams.projectionMatrix, depthRange, dimensions, viewport);

    // Resolve!
    if (_contextParams.resolveMode ==
            HdxPickTokens->resolveNearestToCenter) {
        result.ResolveNearestToCenter(_contextParams.outHits);
    } else if (_contextParams.resolveMode ==
            HdxPickTokens->resolveNearestToCamera) {
        result.ResolveNearestToCamera(_contextParams.outHits);
    } else if (_contextParams.resolveMode ==
            HdxPickTokens->resolveUnique) {
        result.ResolveUnique(_contextParams.outHits);
    } else if (_contextParams.resolveMode ==
            HdxPickTokens->resolveAll) {
        result.ResolveAll(_contextParams.outHits);
    } else {
        TF_CODING_ERROR("Unrecognized interesection mode '%s'",
            _contextParams.resolveMode.GetText());
    }
}

const TfTokenVector &
HdxPickTask::GetRenderTags() const
{
    return _allRenderTags;
}

// -------------------------------------------------------------------------- //
// HdxPickResult
// -------------------------------------------------------------------------- //
HdxPickResult::HdxPickResult(
        int const* primIds,
        int const* instanceIds,
        int const* elementIds,
        int const* edgeIds,
        int const* pointIds,
        int const* neyes,
        float const* depths,
        HdRenderIndex const *index,
        TfToken const& pickTarget,
        GfMatrix4d const& viewMatrix,
        GfMatrix4d const& projectionMatrix,
        GfVec2f const& depthRange,
        GfVec2i const& bufferSize,
        GfVec4i const& subRect)
    : _primIds(primIds)
    , _instanceIds(instanceIds)
    , _elementIds(elementIds)
    , _edgeIds(edgeIds)
    , _pointIds(pointIds)
    , _neyes(neyes)
    , _depths(depths)
    , _index(index)
    , _pickTarget(pickTarget)
    , _depthRange(depthRange)
    , _bufferSize(bufferSize)
    , _subRect(subRect)
{
    // Clamp _subRect [x,y,w,h] to render buffer [0,0,w,h]
    _subRect[0] = std::max(0, _subRect[0]);
    _subRect[1] = std::max(0, _subRect[1]);
    _subRect[2] = std::min(_bufferSize[0]-_subRect[0], _subRect[2]);
    _subRect[3] = std::min(_bufferSize[1]-_subRect[1], _subRect[3]);

    _eyeToWorld = viewMatrix.GetInverse();
    _ndcToWorld = (viewMatrix * projectionMatrix).GetInverse();
}

HdxPickResult::~HdxPickResult() = default;

HdxPickResult::HdxPickResult(HdxPickResult &&) = default;

HdxPickResult&
HdxPickResult::operator=(HdxPickResult &&) = default;

bool
HdxPickResult::IsValid() const
{
    // Make sure we have at least a primId buffer and a depth buffer.
    if (!_depths || !_primIds) {
        return false;
    }

    return true;
}

GfVec3f
HdxPickResult::_GetNormal(int index) const
{
    GfVec3f normal = GfVec3f(0);
    if (_neyes != nullptr) {
        GfVec3f neye =
            HdVec4f_2_10_10_10_REV(_neyes[index]).GetAsVec<GfVec3f>();
        normal = _eyeToWorld.TransformDir(neye);
    }
    return normal;
}

bool
HdxPickResult::_ResolveHit(int index, int x, int y, float z,
                           HdxPickHit* hit) const
{
    int primId = _GetPrimId(index);
    hit->objectId = _index->GetRprimPathFromPrimId(primId);

    if (!hit->IsValid()) {
        return false;
    }

    bool rprimValid = _index->GetSceneDelegateAndInstancerIds(hit->objectId,
                                                           &(hit->delegateId),
                                                           &(hit->instancerId));

    if (!TF_VERIFY(rprimValid, "%s\n", hit->objectId.GetText())) {
        return false;
    }

    // Calculate the hit location in NDC, then transform to worldspace.
    GfVec3d ndcHit(
        ((double)x / _bufferSize[0]) * 2.0 - 1.0,
        ((double)y / _bufferSize[1]) * 2.0 - 1.0,
        ((z - _depthRange[0]) / (_depthRange[1] - _depthRange[0])) * 2.0 - 1.0);
    hit->worldSpaceHitPoint = GfVec3f(_ndcToWorld.Transform(ndcHit));
    hit->worldSpaceHitNormal = _GetNormal(index);
    hit->normalizedDepth =
        (z - _depthRange[0]) / (_depthRange[1] - _depthRange[0]);

    hit->instanceIndex = _GetInstanceId(index);
    hit->elementIndex = _GetElementId(index);
    hit->edgeIndex = _GetEdgeId(index);
    hit->pointIndex = _GetPointId(index);

    if (TfDebug::IsEnabled(HDX_INTERSECT)) {
        std::cout << *hit << std::endl;
    }

    return true;
}

size_t
HdxPickResult::_GetHash(int index) const
{
    size_t hash = 0;
    boost::hash_combine(hash, _GetPrimId(index));
    boost::hash_combine(hash, _GetInstanceId(index));
    if (_pickTarget == HdxPickTokens->pickFaces) {
        boost::hash_combine(hash, _GetElementId(index));
    }
    if (_pickTarget == HdxPickTokens->pickEdges) {
        boost::hash_combine(hash, _GetEdgeId(index));
    }
    if (_pickTarget == HdxPickTokens->pickPoints) {
        boost::hash_combine(hash, _GetPointId(index));
    }
    return hash;
}

bool
HdxPickResult::_IsValidHit(int index) const
{
    // Inspect the id buffers to determine if the pixel index is a valid hit
    // by accounting for the pick target when picking points and edges.
    // This allows the hit(s) returned to be relevant.
    bool validPrim = (_GetPrimId(index) != -1);
    bool invalidTargetEdgePick = (_pickTarget == HdxPickTokens->pickEdges)
        && (_GetEdgeId(index) == -1);
    bool invalidTargetPointPick = (_pickTarget == HdxPickTokens->pickPoints)
        && (_GetPointId(index) == -1);

    return validPrim
           && !invalidTargetEdgePick
           && !invalidTargetPointPick;
}

void
HdxPickResult::ResolveNearestToCamera(HdxPickHitVector* allHits) const
{
    TRACE_FUNCTION();

    if (!IsValid() || !allHits) {
        return;
    }

    int xMin = 0;
    int yMin = 0;
    double zMin = 0;
    int zMinIndex = -1;

    // Find the smallest value (nearest pixel) in the z buffer that is a valid
    // prim. The last part is important since the depth buffer may be
    // populated with occluders (which aren't picked, and thus won't update any
    // of the ID buffers)
    for (int y = _subRect[1]; y < _subRect[1] + _subRect[3]; ++y) {
        for (int x = _subRect[0]; x < _subRect[0] + _subRect[2]; ++x) {
            int i = y * _bufferSize[0] + x;
            if (_IsValidHit(i) && (zMinIndex == -1 || _depths[i] < zMin)) {
                xMin = x;
                yMin = y;
                zMin = _depths[i];
                zMinIndex = i;
            }
        }
    }

    if (zMinIndex == -1) {
        // We didn't find any valid hits.
        return;
    }

    HdxPickHit hit;
    if (_ResolveHit(zMinIndex, xMin, yMin, zMin, &hit)) {
        allHits->push_back(hit);
    }
}

void
HdxPickResult::ResolveNearestToCenter(HdxPickHitVector* allHits) const
{
    TRACE_FUNCTION();

    if (!IsValid() || !allHits) {
        return;
    }

    int width = _subRect[2];
    int height = _subRect[3];

    int midH = height/2;
    int midW = width /2;
    if (height%2 == 0) {
        midH--;
    }
    if (width%2 == 0) {
        midW--;
    }

    // Return the first valid hit that's closest to the center of the draw
    // target by walking from the center outwards.
    for (int w = midW, h = midH; w >= 0 && h >= 0; w--, h--) {
        for (int ww = w; ww < width-w; ww++) {
            for (int hh = h; hh < height-h; hh++) {
                int x = ww + _subRect[0];
                int y = hh + _subRect[1];
                int i = y * _bufferSize[0] + x;
                if (_IsValidHit(i)) {
                    HdxPickHit hit;
                    if (_ResolveHit(i, x, y, _depths[i], &hit)) {
                        allHits->push_back(hit);
                    }
                    return;
                }
                // Skip pixels we've already visited and jump to the boundary
                if (!(ww == w || ww == width-w-1) && hh == h) {
                    hh = std::max(hh, height-h-2);
                }
            }
        }
    }
}

void
HdxPickResult::ResolveAll(HdxPickHitVector* allHits) const
{
    TRACE_FUNCTION();

    if (!IsValid() || !allHits) {
        return;
    }

    for (int y = _subRect[1]; y < _subRect[1] + _subRect[3]; ++y) {
        for (int x = _subRect[0]; x < _subRect[0] + _subRect[2]; ++x) {
            int i = y * _bufferSize[0] + x;
            if (!_IsValidHit(i)) continue;

            HdxPickHit hit;
            if (_ResolveHit(i, x, y, _depths[i], &hit)) {
                allHits->push_back(hit);
            }
        }
    }
}

void
HdxPickResult::ResolveUnique(HdxPickHitVector* allHits) const
{
    TRACE_FUNCTION();

    if (!IsValid() || !allHits) {
        return;
    }

    std::unordered_map<size_t, GfVec2i> hitIndices;
    {
        HD_TRACE_SCOPE("unique indices");
        size_t previousHash = 0;
        for (int y = _subRect[1]; y < _subRect[1] + _subRect[3]; ++y) {
            for (int x = _subRect[0]; x < _subRect[0] + _subRect[2]; ++x) {
                int i = y * _bufferSize[0] + x;
                if (!_IsValidHit(i)) continue;

                size_t hash = _GetHash(i);
                // As an optimization, keep track of the previous hash value and
                // reject indices that match it without performing a map lookup.
                // Adjacent indices are likely enough to have the same prim,
                // instance and if relevant, the same subprim ids, that this can
                // be a significant improvement.
                if (hitIndices.empty() || hash != previousHash) {
                    hitIndices.insert(std::make_pair(hash, GfVec2i(x,y)));
                    previousHash = hash;
                }
            }
        }
    }

    {
        HD_TRACE_SCOPE("resolve");

        for (auto const& pair : hitIndices) {
            int x = pair.second[0];
            int y = pair.second[1];
            int i = y * _bufferSize[0] + x;

            HdxPickHit hit;
            if (_ResolveHit(i, x, y, _depths[i], &hit)) {
                allHits->push_back(hit);
            }
        }
    }
}

// -------------------------------------------------------------------------- //
// HdxPickHit
// -------------------------------------------------------------------------- //
size_t
HdxPickHit::GetHash() const
{
    size_t hash = 0;

    boost::hash_combine(hash, delegateId.GetHash());
    boost::hash_combine(hash, objectId.GetHash());
    boost::hash_combine(hash, instancerId);
    boost::hash_combine(hash, instanceIndex);
    boost::hash_combine(hash, elementIndex);
    boost::hash_combine(hash, edgeIndex);
    boost::hash_combine(hash, pointIndex);
    boost::hash_combine(hash, worldSpaceHitPoint[0]);
    boost::hash_combine(hash, worldSpaceHitPoint[1]);
    boost::hash_combine(hash, worldSpaceHitPoint[2]);
    boost::hash_combine(hash, worldSpaceHitNormal[0]);
    boost::hash_combine(hash, worldSpaceHitNormal[1]);
    boost::hash_combine(hash, worldSpaceHitNormal[2]);
    boost::hash_combine(hash, normalizedDepth);
    
    return hash;
}

// -------------------------------------------------------------------------- //
// Comparison, equality and logging
// -------------------------------------------------------------------------- //
bool
operator<(HdxPickHit const& lhs, HdxPickHit const& rhs)
{
    return lhs.normalizedDepth < rhs.normalizedDepth;
}

bool
operator==(HdxPickHit const& lhs, HdxPickHit const& rhs)
{
    return lhs.objectId == rhs.objectId
        && lhs.delegateId == rhs.delegateId
        && lhs.instancerId == rhs.instancerId
        && lhs.instanceIndex == rhs.instanceIndex
        && lhs.elementIndex == rhs.elementIndex
        && lhs.edgeIndex == rhs.edgeIndex
        && lhs.pointIndex == rhs.pointIndex
        && lhs.worldSpaceHitPoint == rhs.worldSpaceHitPoint
        && lhs.worldSpaceHitNormal == rhs.worldSpaceHitNormal
        && lhs.normalizedDepth == rhs.normalizedDepth;
}

bool
operator!=(HdxPickHit const& lhs, HdxPickHit const& rhs)
{
    return !(lhs==rhs);
}

std::ostream&
operator<<(std::ostream& out, HdxPickHit const& h)
{
    out << "Delegate: <" << h.delegateId << "> "
        << "Object: <" << h.objectId << "> "
        << "Instancer: <" << h.instancerId << "> "
        << "Instance: [" << h.instanceIndex << "] "
        << "Element: [" << h.elementIndex << "] "
        << "Edge: [" << h.edgeIndex  << "] "
        << "Point: [" << h.pointIndex  << "] "
        << "HitPoint: (" << h.worldSpaceHitPoint << ") "
        << "HitNormal: (" << h.worldSpaceHitNormal << ") "
        << "Depth: (" << h.normalizedDepth << ") ";
    return out;
}

bool
operator==(HdxPickTaskParams const& lhs, HdxPickTaskParams const& rhs)
{
    return lhs.cullStyle == rhs.cullStyle
        && lhs.enableSceneMaterials == rhs.enableSceneMaterials;
}

bool
operator!=(HdxPickTaskParams const& lhs, HdxPickTaskParams const& rhs)
{
    return !(lhs==rhs);
}

std::ostream&
operator<<(std::ostream& out, HdxPickTaskParams const& p)
{
    out << "PickTask Params: (...) "
        << p.cullStyle << " "
        << p.enableSceneMaterials;
    return out;
}

bool
operator==(HdxPickTaskContextParams const& lhs,
           HdxPickTaskContextParams const& rhs)
{
    using RawDepthMaskCallback = void (*) ();
    const RawDepthMaskCallback *lhsDepthMaskPtr =
        lhs.depthMaskCallback.target<RawDepthMaskCallback>();
    const RawDepthMaskCallback *rhsDepthMaskPtr =
        rhs.depthMaskCallback.target<RawDepthMaskCallback>();
    const RawDepthMaskCallback lhsDepthMask =
        lhsDepthMaskPtr ? *lhsDepthMaskPtr : nullptr;
    const RawDepthMaskCallback rhsDepthMask =
        rhsDepthMaskPtr ? *rhsDepthMaskPtr : nullptr;

    return lhs.resolution == rhs.resolution
        && lhs.pickTarget == rhs.pickTarget
        && lhs.resolveMode == rhs.resolveMode
        && lhs.doUnpickablesOcclude == rhs.doUnpickablesOcclude
        && lhs.viewMatrix == rhs.viewMatrix
        && lhs.projectionMatrix == rhs.projectionMatrix
        && lhs.clipPlanes == rhs.clipPlanes
        && lhsDepthMask == rhsDepthMask
        && lhs.collection == rhs.collection
        && lhs.outHits == rhs.outHits;
}

bool
operator!=(HdxPickTaskContextParams const& lhs,
           HdxPickTaskContextParams const& rhs)
{
    return !(lhs==rhs);
}

std::ostream&
operator<<(std::ostream& out, HdxPickTaskContextParams const& p)
{
    using RawDepthMaskCallback = void (*) ();
    const RawDepthMaskCallback *depthMaskPtr =
        p.depthMaskCallback.target<RawDepthMaskCallback>();
    const RawDepthMaskCallback depthMask =
        depthMaskPtr ? *depthMaskPtr : nullptr;

    out << "PickTask Context Params: (...) "
        << p.resolution << " "
        << p.pickTarget << " "
        << p.resolveMode << " "
        << p.doUnpickablesOcclude << " "
        << p.viewMatrix << " "
        << p.projectionMatrix << " "
        << depthMask << " "
        << p.collection << " "
        << p.outHits;
    for (auto const& a : p.clipPlanes) {
        out << a << " ";
    }
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
