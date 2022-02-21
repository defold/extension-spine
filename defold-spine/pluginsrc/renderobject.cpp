#include "renderobject.h"
#include <dmsdk/dlib/log.h>

namespace dmSpinePlugin
{

void StencilTestParams::Init()
{
    m_Front.m_Func = dmGraphics::COMPARE_FUNC_ALWAYS;
    m_Front.m_OpSFail = dmGraphics::STENCIL_OP_KEEP;
    m_Front.m_OpDPFail = dmGraphics::STENCIL_OP_KEEP;
    m_Front.m_OpDPPass = dmGraphics::STENCIL_OP_KEEP;
    m_Back.m_Func = dmGraphics::COMPARE_FUNC_ALWAYS;
    m_Back.m_OpSFail = dmGraphics::STENCIL_OP_KEEP;
    m_Back.m_OpDPFail = dmGraphics::STENCIL_OP_KEEP;
    m_Back.m_OpDPPass = dmGraphics::STENCIL_OP_KEEP;
    m_Ref = 0;
    m_RefMask = 0xff;
    m_BufferMask = 0xff;
    m_ColorBufferMask = 0xf;
    m_ClearBuffer = 0;
    m_SeparateFaceStates = 0;
}

void RenderObject::Init()
{
    memset(this, 0, sizeof(*this));

    m_StencilTestParams.Init();

    m_WorldTransform = dmVMath::Matrix4::identity();
    //m_TextureTransform = dmVMath::Matrix4::identity();
}

void RenderObject::AddConstant(dmhash_t name_hash, const dmVMath::Vector4& value)
{
    uint32_t index = RenderObject::MAX_CONSTANT_COUNT;
    for (uint32_t i = 0; i < RenderObject::MAX_CONSTANT_COUNT; ++i)
    {
        if (m_Constants[i].m_NameHash == 0 || m_Constants[i].m_NameHash == name_hash) {
            index = i;
            break;
        }
    }

    if (index < RenderObject::MAX_CONSTANT_COUNT)
    {
        if (m_Constants[index].m_NameHash == 0)
            m_NumConstants++;
        m_Constants[index].m_NameHash = name_hash;
        m_Constants[index].m_Value = value;
    }
    else
    {
        dmLogWarning("Failed to add shader constant %s: %.3f, %.3f, %.3f, %.3f  this: %p  index: %u  num_c: %u",
            dmHashReverseSafe64(name_hash), value.getX(),value.getY(),value.getZ(),value.getW(), this, index, m_NumConstants);
    }
}

}