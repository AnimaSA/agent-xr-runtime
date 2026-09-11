cbuffer Constants : register(b0)
{
    float4 uvTransform;
    float4 ndcRect;
    uint arraySlice;
    uint blendSourceAlpha;
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
    color.a = blendSourceAlpha != 0 ? color.a * alpha : alpha;
    return color;
}
