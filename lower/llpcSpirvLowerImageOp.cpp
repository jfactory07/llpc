/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcSpirvLowerImageOp.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerImageOp.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-spirv-lower-image-op"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerImageOp.h"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerImageOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for image operations
ModulePass* CreateSpirvLowerImageOp()
{
    return new SpirvLowerImageOp();
}

// =====================================================================================================================
SpirvLowerImageOp::SpirvLowerImageOp()
    :
    SpirvLower(ID),
    m_restoreMeta(false)
{
    initializePipelineShadersPass(*PassRegistry::getPassRegistry());
    initializeSpirvLowerImageOpPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerImageOp::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Image-Op\n");

    SpirvLower::Init(&module);

    // Process each shader stage in turn.
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (uint32_t shaderStage = 0; shaderStage < ShaderStageCountInternal; ++shaderStage)
    {
        m_pEntryPoint = pPipelineShaders->GetEntryPoint(ShaderStage(shaderStage));
        if (m_pEntryPoint != nullptr)
        {
            m_shaderStage = ShaderStage(shaderStage);
            ProcessShader();
        }
    }

    return true;
}

// =====================================================================================================================
// Process one shader stage
void SpirvLowerImageOp::ProcessShader()
{
    // Visit module to restore per-instruction metadata
    m_restoreMeta = true;
    visit(m_pEntryPoint);
    m_restoreMeta = false;

    // Invoke handling of "call" instruction
    visit(m_pEntryPoint);

    for (auto pCallInst: m_imageCalls)
    {
        pCallInst->dropAllReferences();
        pCallInst->eraseFromParent();
    }
    m_imageCalls.clear();

    for (auto pInst : m_imageLoads)
    {
        if (pInst->use_empty())
        {
            pInst->dropAllReferences();
            pInst->eraseFromParent();
            m_imageLoadOperands.erase(pInst);
        }
    }
    m_imageLoads.clear();

    // NOTE: The set of image load operands is the operands of image load instructions. We must free image load
    // instructions first. Otherwise, the user of those image load operands will not be empty.
    for (auto pOperand : m_imageLoadOperands)
    {
        if (pOperand->use_empty())
        {
            pOperand->dropAllReferences();
            pOperand->eraseFromParent();
        }
    }
    m_imageLoadOperands.clear();
}

// =====================================================================================================================
// Visits "call" instruction.
void SpirvLowerImageOp::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    // Skip image lowering operations except entry-points
    if (callInst.getParent()->getParent()->getLinkage() == GlobalValue::InternalLinkage)
    {
        return;
    }

    if (m_restoreMeta)
    {
        // Restore non-uniform metadata from metadata instruction.
        LLPC_ASSERT(strlen(gSPIRVMD::NonUniform) == 16);
        const std::string NonUniformPrefix = std::string("_Z16") + std::string(gSPIRVMD::NonUniform);
        if (pCallee->getName().startswith(NonUniformPrefix))
        {
            auto pNonUniform = callInst.getOperand(0);
            cast<Instruction>(pNonUniform)->setMetadata(gSPIRVMD::NonUniform, m_pContext->GetEmptyMetadataNode());
        }
        return;
    }

    bool isUndefImage = false;

    if (pCallee->getName().startswith(gSPIRVName::ImageCallPrefix))
    {
        ShaderImageCallMetadata imageCallMeta = {};
        LLPC_ASSERT(callInst.getNumArgOperands() >= 2);
        uint32_t metaOperandIdx = callInst.getNumArgOperands() - 1; // Image call metadata is last argument
        imageCallMeta.U32All =  cast<ConstantInt>(callInst.getArgOperand(metaOperandIdx))->getZExtValue();

        if ((imageCallMeta.OpKind == ImageOpWrite) || isImageAtomicOp(imageCallMeta.OpKind))
        {
            m_pContext->GetShaderResourceUsage(m_shaderStage)->resourceWrite = true;
        }
        else if (imageCallMeta.OpKind == ImageOpRead)
        {
            m_pContext->GetShaderResourceUsage(m_shaderStage)->resourceRead = true;
        }

        ConstantInt* pMemoryQualifier = nullptr;
        ConstantInt* pResourceDescSet = nullptr;
        ConstantInt* pResourceBinding = nullptr;
        ConstantInt* pSamplerDescSet  = nullptr;
        ConstantInt* pSamplerBinding  = nullptr;
        Value* pResourceIndex = nullptr;
        Value* pSamplerIndex  = nullptr;

        std::string mangledName;

        if (isa<LoadInst>(callInst.getOperand(0)))
        {
            // Combined resource and sampler
            auto pLoadCombined = cast<LoadInst>(callInst.getOperand(0));
            ExtractBindingInfo(pLoadCombined,
                               &pResourceDescSet,
                               &pResourceBinding,
                               &pResourceIndex,
                               &pMemoryQualifier);

            // Descriptor set and binging of sampler are the same as those of resource
            pSamplerDescSet = pResourceDescSet;
            pSamplerBinding = pResourceBinding;
            pSamplerIndex   = pResourceIndex;

            m_imageLoads.insert(pLoadCombined);
        }
        else if (isa<CallInst>(callInst.getOperand(0)))
        {
            auto pLoadCall = cast<CallInst>(callInst.getOperand(0));
            mangledName = pLoadCall->getCalledFunction()->getName();
            if (mangledName.find("_Z12SampledImage") == 0)
            {
                // Seperated resource and sampler (from SPIR-V "OpSampledImage")
                if (isa<UndefValue>(pLoadCall->getOperand(0)) || isa<UndefValue>(pLoadCall->getOperand(1)))
                {
                    isUndefImage = true;
                    m_imageLoads.insert(pLoadCall);
                }
                else
                {
                    auto pLoadResource = cast<LoadInst>(pLoadCall->getOperand(0));
                    auto pLoadSampler  = cast<LoadInst>(pLoadCall->getOperand(1));

                    ExtractBindingInfo(pLoadResource,
                                       &pResourceDescSet,
                                       &pResourceBinding,
                                       &pResourceIndex,
                                       &pMemoryQualifier);

                    ExtractBindingInfo(pLoadSampler,
                                       &pSamplerDescSet,
                                       &pSamplerBinding,
                                       &pSamplerIndex,
                                       &pMemoryQualifier);

                    m_imageLoads.insert(pLoadCall);

                    m_imageLoadOperands.insert(pLoadResource);
                    m_imageLoadOperands.insert(pLoadSampler);
                }
            }
            else if (mangledName.find("_Z5Image") == 0)
            {
                // Resource only (from SPIR-V "OpImage")
                if (isa<UndefValue>(pLoadCall->getOperand(0)))
                {
                    isUndefImage = true;
                    m_imageLoads.insert(pLoadCall);
                }
                else if (isa<LoadInst>(pLoadCall->getOperand(0)))
                {
                    // Extract resource from load instruction
                    auto pLoadResource = cast<LoadInst>(pLoadCall->getOperand(0));
                    ExtractBindingInfo(pLoadResource,
                                       &pResourceDescSet,
                                       &pResourceBinding,
                                       &pResourceIndex,
                                       &pMemoryQualifier);

                    m_imageLoadOperands.insert(pLoadResource);
                    m_imageLoads.insert(pLoadCall);
                }
                else
                {
                    // Extract resource from separated resource and sampler (from SPIR-V "OpSampledImage")
                    LLPC_ASSERT(isa<CallInst>(pLoadCall->getOperand(0)));
                    m_imageLoads.insert(pLoadCall);
                    pLoadCall = cast<CallInst>(pLoadCall->getOperand(0));

                    mangledName = pLoadCall->getCalledFunction()->getName();
                    LLPC_ASSERT(mangledName.find("_Z12SampledImage") == 0);

                    auto pLoadResource = cast<LoadInst>(pLoadCall->getOperand(0));
                    ExtractBindingInfo(pLoadResource,
                                       &pResourceDescSet,
                                       &pResourceBinding,
                                       &pResourceIndex,
                                       &pMemoryQualifier);

                    m_imageLoadOperands.insert(pLoadCall);
                }
            }
        }
        else if (isa<UndefValue>(callInst.getOperand(0)))
        {
            isUndefImage = true;
        }

        if (isUndefImage)
        {
            // Replace undef-image call with undefined value.
            auto pUndef = UndefValue::get(callInst.getType());
            callInst.replaceAllUsesWith(pUndef);
            m_imageCalls.insert(&callInst);
        }
        else
        {
            mangledName = pCallee->getName();

            std::vector<Value*> args;

            if ((imageCallMeta.OpKind == ImageOpSample) ||
                (imageCallMeta.OpKind == ImageOpGather) ||
                (imageCallMeta.OpKind == ImageOpQueryLod))
            {
                // Add sampler only for image sample and image gather
                args.push_back(pSamplerDescSet);
                args.push_back(pSamplerBinding);
                args.push_back(pSamplerIndex);
                std::unordered_set<Value*> checkedValuesSampler;
                imageCallMeta.NonUniformSampler = IsNonUniformValue(pSamplerIndex, checkedValuesSampler) ? 1 : 0;
            }

            args.push_back(pResourceDescSet);
            args.push_back(pResourceBinding);
            args.push_back(pResourceIndex);
            std::unordered_set<Value*> checkedValuesResource;
            imageCallMeta.NonUniformResource = IsNonUniformValue(pResourceIndex, checkedValuesResource) ? 1 : 0;
            imageCallMeta.WriteOnly = callInst.getType()->isVoidTy();

            if (imageCallMeta.OpKind != ImageOpQueryNonLod)
            {
                // NOTE: Here, we reduce the size of coordinate to its actual size. According to SPIR-V spec, coordinate
                // is allowed to be a vector larger than needed, this will cause LLVM type mismatch when linking.
                Dim dim = static_cast<Dim>(imageCallMeta.Dim);
                uint32_t requiredCompCount;
                SPIRVDimCoordNumMap::find(dim, &requiredCompCount);
                if (imageCallMeta.Arrayed)
                {
                    ++requiredCompCount;
                }
                if (mangledName.find(gSPIRVName::ImageCallModProj) != std::string::npos)
                {
                    ++requiredCompCount;
                }

                Value* pCoord = callInst.getArgOperand(1);
                Type* pCoordTy = pCoord->getType();

                bool coordIsVec = pCoordTy->isVectorTy();
                uint32_t coordCompCount = coordIsVec ? pCoordTy->getVectorNumElements() : 1;
                Type* pCoordCompTy = coordIsVec ? pCoordTy->getVectorElementType() : pCoordTy;

                if (coordCompCount > requiredCompCount)
                {
                    // Need vector size reduction for coordinate
                    VectorType* pNewCoordTy = VectorType::get(pCoordCompTy, requiredCompCount);
                    Value* coordComps[] = { nullptr, nullptr, nullptr, nullptr };
                    for (uint32_t i = 0; i < requiredCompCount; ++i)
                    {
                        coordComps[i] = ExtractElementInst::Create(pCoord,
                                                                   ConstantInt::get(m_pContext->Int32Ty(), i, true),
                                                                   "",
                                                                   &callInst);
                    }

                    if (requiredCompCount == 1)
                    {
                        args.push_back(coordComps[0]);
                    }
                    else
                    {
                        Value* pNewCoord = UndefValue::get(pNewCoordTy);
                        for (uint32_t i = 0; i < requiredCompCount; ++i)
                        {
                            pNewCoord = InsertElementInst::Create(pNewCoord,
                                                                  coordComps[i],
                                                                  ConstantInt::get(m_pContext->Int32Ty(), i, true),
                                                                  "",
                                                                  &callInst);
                        }
                        args.push_back(pNewCoord);
                    }
                }
                else
                {
                    if (dim == DimSubpassData)
                    {
                        LLPC_ASSERT(m_shaderStage == ShaderStageFragment);
                        const auto enableMultiView = (reinterpret_cast<const GraphicsPipelineBuildInfo*>(
                            m_pContext->GetPipelineBuildInfo()))->iaState.enableMultiView;

                        if (enableMultiView)
                        {
                            auto pInt32Ty = m_pContext->Int32Ty();
                            auto pBuiltInViewIndex = ConstantInt::get(pInt32Ty, BuiltInViewIndex);
                            auto instName = std::string(LlpcName::InputImportBuiltIn) + "ViewIndex";
                            AddTypeMangling(pInt32Ty, pBuiltInViewIndex, instName);
                            auto pModule = pCallee->getParent();
                            auto pViewIndex = EmitCall(pModule,
                                                       instName,
                                                       pInt32Ty,
                                                       pBuiltInViewIndex,
                                                       NoAttrib,
                                                       &callInst);
                            pCoord = InsertElementInst::Create(pCoord,
                                                               pViewIndex,
                                                               ConstantInt::get(m_pContext->Int32Ty(), 0, true),
                                                               "",
                                                               &callInst);
                        }
                    }
                    args.push_back(pCoord);
                }

                for (uint32_t i = 2; i < callInst.getNumArgOperands() - 1; ++i)
                {
                    auto pArg = callInst.getArgOperand(i);
                    args.push_back(pArg);
                }
                // ImageCallMeta may be changed due to non-uniform index, so we can't copy it from callInst.
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), imageCallMeta.U32All));
            }
            else
            {
                for (uint32_t i = 1; i < callInst.getNumArgOperands() - 1; ++i)
                {
                    auto pArg = callInst.getArgOperand(i);
                    args.push_back(pArg);
                }
                // ImageCallMeta may be changed due to non-uniform index, so we can't copy it from callInst.
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), imageCallMeta.U32All));
            }

            // Process image memory metadata
            if ((imageCallMeta.OpKind == ImageOpRead) || (imageCallMeta.OpKind == ImageOpWrite))
            {
                LLPC_ASSERT(pMemoryQualifier != nullptr); // Must be present
                ShaderImageMemoryMetadata imageMemoryMeta = {};
                imageMemoryMeta.U32All = pMemoryQualifier->getZExtValue();
                args.pop_back();
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), imageMemoryMeta.Coherent ? true : false)); // glc
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), imageMemoryMeta.Volatile ? true : false)); // slc
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), imageCallMeta.U32All)); //imageCallMeta
            }
            else if (isImageAtomicOp(imageCallMeta.OpKind))
            {
                LLPC_ASSERT(pMemoryQualifier != nullptr); // Must be present
                ShaderImageMemoryMetadata imageMemoryMeta = {};
                imageMemoryMeta.U32All = pMemoryQualifier->getZExtValue();
                args.pop_back();
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), imageMemoryMeta.Volatile ? true : false)); // slc
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), imageCallMeta.U32All)); //imageCallMeta
            }

            if (imageCallMeta.OpKind == ImageOpQueryNonLod)
            {
                // Format: ...".query.op.imagesig.dim[.rettype]"...
                size_t pos = mangledName.find(gSPIRVName::ImageCallQueryNonLodPrefix);
                LLPC_ASSERT(pos != std::string::npos);

                // Skip the query operation name
                pos = mangledName.find(".", pos + 1);

                // Find the name string for image signature and remove it
                size_t startPos = mangledName.find(".", pos + 1);
                size_t endPos   = mangledName.find(".", startPos + 1);
                if (endPos == std::string::npos)
                {
                    endPos = mangledName.length();
                }
                mangledName = mangledName.substr(0, startPos) + mangledName.substr(endPos);
            }
            else if (imageCallMeta.OpKind == ImageOpQueryLod)
            {
                // NOTE: "Array" and "shadow" modifiers do not have real impacts.
                // They are only to keep function uniqueness (avoid overloading).
                // Format: ...".querylod.dim"
                size_t pos = mangledName.find(gSPIRVName::ImageCallQueryLodPrefix);
                LLPC_ASSERT(pos != std::string::npos);
                pos = mangledName.find(".", pos + 1);
                size_t modPos = mangledName.find("Array", pos + 1);
                if (modPos != std::string::npos)
                {
                    mangledName = mangledName.substr(0, modPos);
                }

                modPos = mangledName.find("Shadow", pos);
                if (modPos != std::string::npos)
                {
                    mangledName = mangledName.substr(0, modPos);
                }
            }

            // Change the name prefix of image call (from "spirv.image" to "llpc.image")
            StringRef prefix(gSPIRVName::ImageCallPrefix);
            std::string callName = LlpcName::ImageCallPrefix + mangledName.substr(prefix.size());

            if (imageCallMeta.Dim != DimBuffer)
            {
                callName += gSPIRVName::ImageCallDimAwareSuffix;
            }

            // Image call replacement
            CallInst* pImageCall =
                cast<CallInst>(EmitCall(m_pModule, callName, callInst.getType(), args, NoAttrib, &callInst));
            callInst.replaceAllUsesWith(pImageCall);

            m_imageCalls.insert(&callInst);
        }
    }
}

// =====================================================================================================================
// Extracts binding info from the specified "load" instruction
void SpirvLowerImageOp::ExtractBindingInfo(
    LoadInst*     pLoadInst,            // [in] "Load" instruction
    ConstantInt** ppDescSet,            // [out] Descriptor set
    ConstantInt** ppBinding,            // [out] Descriptor binding
    Value**       ppArrayIndex,         // [out] Descriptor index
    ConstantInt** ppMemoryQualifier)    // [out] Memory qualifier
{
    Value* pLoadSrc = pLoadInst->getOperand(0);
    MDNode* pResMetaNode = nullptr;
    MDNode* pImageMemoryMetaNode = nullptr;

    GetElementPtrInst* pGetElemPtrInst = nullptr;
    Instruction* pConstExpr = nullptr;

    if (isa<GetElementPtrInst>(pLoadSrc))
    {
        pGetElemPtrInst = dyn_cast<GetElementPtrInst>(pLoadSrc);
    }
    else if (isa<ConstantExpr>(pLoadSrc))
    {
        pConstExpr = dyn_cast<ConstantExpr>(pLoadSrc)->getAsInstruction();
        pGetElemPtrInst = dyn_cast<GetElementPtrInst>(pConstExpr);
    }

    // Calculate descriptor index for arrayed binding
    if (pGetElemPtrInst != nullptr)
    {
        // Process image array access

        // Get stride of each array dimension
        std::vector<uint32_t> strides;
        Type* pSourceTy = pGetElemPtrInst->getSourceElementType();
        LLPC_ASSERT(pSourceTy->isArrayTy());

        Type* pElemTy = pSourceTy->getArrayElementType();
        while (pElemTy->isArrayTy())
        {
            const uint32_t elemCount = pElemTy->getArrayNumElements();
            for (uint32_t i = 0; i < strides.size(); ++i)
            {
                strides[i] *= elemCount;
            }

            strides.push_back(elemCount);
            pElemTy = pElemTy->getArrayElementType();
        }
        strides.push_back(1);

        // Calculate flatten array index
        const uint32_t operandCount = pGetElemPtrInst->getNumOperands();
        LLPC_ASSERT((operandCount - 2) == strides.size());

        Value* pArrayIndex = nullptr;
        for (uint32_t i = 2; i < operandCount; ++i)
        {
            Value* pIndex = pGetElemPtrInst->getOperand(i);
            bool isType64 = (pIndex->getType()->getPrimitiveSizeInBits() == 64);
            Constant* pStride = ConstantInt::get(m_pContext->Int32Ty(), strides[i-2]);

            if (isType64)
            {
                pIndex =  new TruncInst(pIndex, m_pContext->Int32Ty(), "", pLoadInst);
            }
            pIndex = BinaryOperator::CreateMul(pStride, pIndex, "", pLoadInst);
            if (pArrayIndex == nullptr)
            {
                pArrayIndex = pIndex;
            }
            else
            {
                pArrayIndex = BinaryOperator::CreateAdd(pArrayIndex, pIndex, "", pLoadInst);
            }
        }

        *ppArrayIndex = pArrayIndex;

        // Get resource binding metadata node from global variable
        Value*  pSource      = pGetElemPtrInst->getPointerOperand();
        pResMetaNode = cast<GlobalVariable>(pSource)->getMetadata(gSPIRVMD::Resource);
        pImageMemoryMetaNode = cast<GlobalVariable>(pSource)->getMetadata(gSPIRVMD::ImageMemory);
    }
    else
    {
        // Load image from global variable
        *ppArrayIndex = ConstantInt::get(m_pContext->Int32Ty(), 0);

        // Get resource binding metadata node from global variable
        pResMetaNode = cast<GlobalVariable>(pLoadSrc)->getMetadata(gSPIRVMD::Resource);
        pImageMemoryMetaNode = cast<GlobalVariable>(pLoadSrc)->getMetadata(gSPIRVMD::ImageMemory);
    }

    if (pConstExpr != nullptr)
    {
        pConstExpr->dropAllReferences();
        pConstExpr->deleteValue();
    }

    // Get descriptor set and descriptor binding
    LLPC_ASSERT(pResMetaNode != nullptr);

    *ppDescSet = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0));
    *ppBinding = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1));

    if (pImageMemoryMetaNode != nullptr)
    {
        *ppMemoryQualifier = mdconst::dyn_extract<ConstantInt>(pImageMemoryMetaNode->getOperand(0));
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for image operations.
INITIALIZE_PASS(SpirvLowerImageOp, DEBUG_TYPE,
                "Lower SPIR-V image operations (sample, fetch, gather, read/write)", false, false)
