#ifndef _VERTEX_LAYOUT_GLSL_
#define _VERTEX_LAYOUT_GLSL_

#ifdef URHO3D_VERTEX_SHADER

// Vertex position
VERTEX_INPUT(vec4 iPos)

// Vertex blending for skeletal animation
#ifdef URHO3D_GEOMETRY_SKINNED
    VERTEX_INPUT(vec4 iBlendWeights)
    VERTEX_INPUT(ivec4_attrib iBlendIndices)
#endif

// Tangent, Normal, Binormal
#ifdef URHO3D_VERTEX_HAS_NORMAL
    VERTEX_INPUT(vec3 iNormal)
#endif
#ifdef URHO3D_VERTEX_HAS_TANGENT
    VERTEX_INPUT(vec4 iTangent)
#endif

// Texture coordinates
#ifdef URHO3D_VERTEX_HAS_TEXCOORD0
    VERTEX_INPUT(vec2 iTexCoord)
#endif
#ifdef URHO3D_VERTEX_HAS_TEXCOORD1
    VERTEX_INPUT(vec2 iTexCoord1)
#endif

// Vertex color
#ifdef URHO3D_VERTEX_HAS_COLOR
    VERTEX_INPUT(vec4 iColor)
#endif

#endif

#endif