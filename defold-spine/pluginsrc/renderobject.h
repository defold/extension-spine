#pragma once

#include <stdint.h>
#include <dmsdk/dlib/vmath.h>
#include <dmsdk/dlib/hash.h>
#include <dmsdk/graphics/graphics.h>
#include <dmsdk/render/render.h>

namespace dmSpinePlugin
{
    // We use a simpler version here, since we don't know the location of the variable in the shader
    struct ShaderConstant
    {
        dmVMath::Vector4 m_Value;
        dmhash_t         m_NameHash;
        uint8_t          pad[8];
    };

    // Due to the fact that the struct has bit fields, it's not possible to get a 1:1 mapping using JNA
    // See dmsdk/render/render.h for the actual implementation
    struct StencilTestParams
    {
        void Init();

        struct
        {
            dmGraphics::CompareFunc m_Func;
            dmGraphics::StencilOp   m_OpSFail;
            dmGraphics::StencilOp   m_OpDPFail;
            dmGraphics::StencilOp   m_OpDPPass;
        } m_Front;

        struct
        {
            dmGraphics::CompareFunc m_Func;
            dmGraphics::StencilOp   m_OpSFail;
            dmGraphics::StencilOp   m_OpDPFail;
            dmGraphics::StencilOp   m_OpDPPass;
        } m_Back;
        // 32 bytes
        uint8_t m_Ref;
        uint8_t m_RefMask;
        uint8_t m_BufferMask;
        uint8_t m_ColorBufferMask;
        bool    m_ClearBuffer;
        bool    m_SeparateFaceStates;
        uint8_t pad[32 - 6];
    };

    // See dmsdk/render/render.h for the actual implementation
    // similar to dmRender::RenderObject, but used for the Editor
    // Matches 1:1 with the Java implementation in the Spine.java
    struct RenderObject
    {
        void Init();
        void AddConstant(dmhash_t name_hash, const dmVMath::Vector4& value);

        static const uint32_t MAX_CONSTANT_COUNT = 4;

        StencilTestParams               m_StencilTestParams;
        dmVMath::Matrix4                m_WorldTransform;
        ShaderConstant                  m_Constants[MAX_CONSTANT_COUNT];
        // 256 bytes
        uint32_t                        m_NumConstants;
        uint32_t                        m_VertexStart;
        uint32_t                        m_VertexCount;
        uint32_t                        : 32;

        bool                            m_SetBlendFactors;
        bool                            m_SetStencilTest;
        bool                            m_SetFaceWinding;
        bool                            m_FaceWindingCCW;
        bool                            m_IsTriangleStrip;
        uint8_t                         pad[(4*4) - 5];
    };

} // dmPlugin
