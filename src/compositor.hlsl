cbuffer Constants : register(b0)
{
    float4 uvTransform;
    float4 ndcRect;
    uint arraySlice;
    uint layerFlags;
    float alpha;
    float padding;
};
Texture2DArray<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 positions[4] = { float2(ndcRect.x, ndcRect.y), float2(ndcRect.z, ndcRect.y), float2(ndcRect.x, ndcRect.w), float2(ndcRect.z, ndcRect.w) };
    float2 uvs[4] = { float2(uvTransform.z, uvTransform.w), float2(uvTransform.z + uvTransform.x, uvTransform.w), float2(uvTransform.z, uvTransform.w + uvTransform.y), float2(uvTransform.z + uvTransform.x, uvTransform.w + uvTransform.y) };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 color = sourceTexture.Sample(sourceSampler, float3(uv, arraySlice));
    const float sourceAlpha = color.a;
    if ((layerFlags & 0x00000002u) == 0u)
    {
        color.a = 1.0f;
    }
    else
    {
        if ((layerFlags & 0x00000004u) != 0u)
        {
            color.rgb *= sourceAlpha;
        }
        color.rgb *= alpha;
        color.a = sourceAlpha * alpha;
    }
    return color;
}
