/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <Atom/Feature/Mesh/MeshFeatureProcessor.h>
#include <Atom/RHI/BufferFrameAttachment.h>
#include <Atom/RHI/BufferScopeAttachment.h>
#include <Atom/RHI/CommandList.h>
#include <Atom/RHI/FrameScheduler.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <Atom/RPI.Public/Buffer/BufferSystemInterface.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/Scene.h>
#include <RayTracing/RayTracingAccelerationStructurePass.h>
#include <RayTracing/RayTracingFeatureProcessor.h>

namespace AZ
{
    namespace Render
    {
        RPI::Ptr<RayTracingAccelerationStructurePass> RayTracingAccelerationStructurePass::Create(const RPI::PassDescriptor& descriptor)
        {
            RPI::Ptr<RayTracingAccelerationStructurePass> rayTracingAccelerationStructurePass = aznew RayTracingAccelerationStructurePass(descriptor);
            return AZStd::move(rayTracingAccelerationStructurePass);
        }

        RayTracingAccelerationStructurePass::RayTracingAccelerationStructurePass(const RPI::PassDescriptor& descriptor)
            : Pass(descriptor)
        {
            // disable this pass if we're on a platform that doesn't support raytracing
            RHI::Ptr<RHI::Device> device = RHI::RHISystemInterface::Get()->GetDevice();
            if (device->GetFeatures().m_rayTracing == false)
            {
                SetEnabled(false);
            }
        }

        void RayTracingAccelerationStructurePass::BuildInternal()
        {
            InitScope(RHI::ScopeId(GetPathName()));
        }

        void RayTracingAccelerationStructurePass::FrameBeginInternal(FramePrepareParams params)
        {
            params.m_frameGraphBuilder->ImportScopeProducer(*this);
        }

        void RayTracingAccelerationStructurePass::SetupFrameGraphDependencies(RHI::FrameGraphInterface frameGraph)
        {
            RHI::Ptr<RHI::Device> device = RHI::RHISystemInterface::Get()->GetDevice();

            RPI::Scene* scene = m_pipeline->GetScene();
            RayTracingFeatureProcessor* rayTracingFeatureProcessor = scene->GetFeatureProcessor<RayTracingFeatureProcessor>();

            if (rayTracingFeatureProcessor)
            {
                if (rayTracingFeatureProcessor->GetRevision() != m_rayTracingRevision)
                {
                    RHI::RayTracingBufferPools& rayTracingBufferPools = rayTracingFeatureProcessor->GetBufferPools();
                    RayTracingFeatureProcessor::SubMeshVector& subMeshes = rayTracingFeatureProcessor->GetSubMeshes();
                    uint32_t rayTracingSubMeshCount = rayTracingFeatureProcessor->GetSubMeshCount();

                    // create the TLAS descriptor
                    RHI::RayTracingTlasDescriptor tlasDescriptor;
                    RHI::RayTracingTlasDescriptor* tlasDescriptorBuild = tlasDescriptor.Build();

                    uint32_t instanceIndex = 0;
                    for (auto& subMesh : subMeshes)
                    {
                        tlasDescriptorBuild->Instance()
                            ->InstanceID(instanceIndex)
                            ->InstanceMask(subMesh.m_mesh->m_instanceMask)
                            ->HitGroupIndex(0)
                            ->Blas(subMesh.m_blas)
                            ->Transform(subMesh.m_mesh->m_transform)
                            ->NonUniformScale(subMesh.m_mesh->m_nonUniformScale)
                            ->Transparent(subMesh.m_irradianceColor.GetA() < 1.0f)
                            ;

                        instanceIndex++;
                    }

                    // create the TLAS buffers based on the descriptor
                    RHI::Ptr<RHI::RayTracingTlas>& rayTracingTlas = rayTracingFeatureProcessor->GetTlas();
                    rayTracingTlas->CreateBuffers(*device, &tlasDescriptor, rayTracingBufferPools);

                    // import and attach the TLAS buffer
                    const RHI::Ptr<RHI::Buffer>& rayTracingTlasBuffer = rayTracingTlas->GetTlasBuffer();
                    if (rayTracingTlasBuffer && rayTracingSubMeshCount)
                    {
                        AZ::RHI::AttachmentId tlasAttachmentId = rayTracingFeatureProcessor->GetTlasAttachmentId();
                        if (frameGraph.GetAttachmentDatabase().IsAttachmentValid(tlasAttachmentId) == false)
                        {
                            [[maybe_unused]] RHI::ResultCode result = frameGraph.GetAttachmentDatabase().ImportBuffer(tlasAttachmentId, rayTracingTlasBuffer);
                            AZ_Assert(result == RHI::ResultCode::Success, "Failed to import ray tracing TLAS buffer with error %d", result);
                        }

                        uint32_t tlasBufferByteCount = aznumeric_cast<uint32_t>(rayTracingTlasBuffer->GetDescriptor().m_byteCount);
                        RHI::BufferViewDescriptor tlasBufferViewDescriptor = RHI::BufferViewDescriptor::CreateRayTracingTLAS(tlasBufferByteCount);

                        RHI::BufferScopeAttachmentDescriptor desc;
                        desc.m_attachmentId = tlasAttachmentId;
                        desc.m_bufferViewDescriptor = tlasBufferViewDescriptor;
                        desc.m_loadStoreAction.m_loadAction = AZ::RHI::AttachmentLoadAction::DontCare;

                        frameGraph.UseShaderAttachment(desc, RHI::ScopeAttachmentAccess::Write);
                    }
                }

                // Attach output data from the skinning pass. This is needed to ensure that this pass is executed after
                // the skinning pass has finished. We assume that the pipeline has a skinning pass with this output available.
                if (rayTracingFeatureProcessor->GetSkinnedMeshCount() > 0)
                {
                    auto skinningPassPtr = FindAdjacentPass(AZ::Name("SkinningPass"));
                    auto skinnedMeshOutputStreamBindingPtr = skinningPassPtr->FindAttachmentBinding(AZ::Name("SkinnedMeshOutputStream"));
                    [[maybe_unused]] auto result = frameGraph.UseShaderAttachment(skinnedMeshOutputStreamBindingPtr->m_unifiedScopeDesc.GetAsBuffer(), RHI::ScopeAttachmentAccess::Read);
                    AZ_Assert(result == AZ::RHI::ResultCode::Success, "Failed to attach SkinnedMeshOutputStream buffer with error %d", result);
                }

                // update and compile the RayTracingSceneSrg and RayTracingMaterialSrg
                // Note: the timing of this update is very important, it needs to be updated after the TLAS is allocated so it can
                // be set on the RayTracingSceneSrg for this frame, and the ray tracing mesh data in the RayTracingSceneSrg must
                // exactly match the TLAS.  Any mismatch in this data may result in a TDR.
                rayTracingFeatureProcessor->UpdateRayTracingSrgs();
            }
        }

        void RayTracingAccelerationStructurePass::BuildCommandList(const RHI::FrameGraphExecuteContext& context)
        {
            RPI::Scene* scene = m_pipeline->GetScene();
            RayTracingFeatureProcessor* rayTracingFeatureProcessor = scene->GetFeatureProcessor<RayTracingFeatureProcessor>();

            if (!rayTracingFeatureProcessor)
            {
                return;
            }

            if (!rayTracingFeatureProcessor->GetTlas()->GetTlasBuffer())
            {
                return;
            }

            if (rayTracingFeatureProcessor->GetRevision() == m_rayTracingRevision && rayTracingFeatureProcessor->GetSkinnedMeshCount() == 0)
            {
                // TLAS is up to date
                return;
            }

            // update the stored revision, even if we don't have any meshes to process
            m_rayTracingRevision = rayTracingFeatureProcessor->GetRevision();

            if (!rayTracingFeatureProcessor->GetSubMeshCount())
            {
                // no ray tracing meshes in the scene
                return;
            }

            // build newly added or skinned BLAS objects
            RayTracingFeatureProcessor::BlasInstanceMap& blasInstances = rayTracingFeatureProcessor->GetBlasInstances();
            for (auto& blasInstance : blasInstances)
            {
                const bool isSkinnedMesh = blasInstance.second.m_isSkinnedMesh;
                if (blasInstance.second.m_blasBuilt == false || isSkinnedMesh)
                {
                    for (auto submeshIndex = 0; submeshIndex < blasInstance.second.m_subMeshes.size(); ++submeshIndex)
                    {
                        auto& submeshBlasInstance = blasInstance.second.m_subMeshes[submeshIndex];
                        if (blasInstance.second.m_blasBuilt == false)
                        {
                            // Always build the BLAS, if it has not previously been built
                            context.GetCommandList()->BuildBottomLevelAccelerationStructure(*submeshBlasInstance.m_blas);
                            continue;
                        }

                        // Determine if a skinned mesh BLAS needs to be updated or completely rebuilt. For now, we want to rebuild a BLAS every
                        // SKINNED_BLAS_REBUILD_FRAME_INTERVAL frames, while updating it all other frames. This is based on the assumption that
                        // by adding together the asset ID hash, submesh index, and frame count, we get a value that allows us to uniformly
                        // distribute rebuilding all skinned mesh BLASs over all frames.
                        auto assetGuid = blasInstance.first.m_guid.GetHash();
                        if (isSkinnedMesh && (assetGuid + submeshIndex + m_frameCount) % SKINNED_BLAS_REBUILD_FRAME_INTERVAL != 0)
                        {
                            // Skinned mesh that simply needs an update
                            context.GetCommandList()->UpdateBottomLevelAccelerationStructure(*submeshBlasInstance.m_blas);
                        }
                        else
                        {
                            // Fall back to building the BLAS in any case
                            context.GetCommandList()->BuildBottomLevelAccelerationStructure(*submeshBlasInstance.m_blas);
                        }
                    }

                    blasInstance.second.m_blasBuilt = true;
                }
            }

            // build the TLAS object
            context.GetCommandList()->BuildTopLevelAccelerationStructure(*rayTracingFeatureProcessor->GetTlas());

            ++m_frameCount;
        }
    }   // namespace RPI
}   // namespace AZ
