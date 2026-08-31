module common;

public struct TonemapParameters
{
    public float exposure;
    public float gamma;
};

// From http://filmicworlds.com/blog/filmic-tonemapping-operators/
float3 Uncharted2Tonemap(float3 color)
{
	float A = 0.15;
	float B = 0.50;
	float C = 0.10;
	float D = 0.20;
	float E = 0.02;
	float F = 0.30;
	float W = 11.2;
	return ((color*(A*color+C*B)+D*E)/(color*(A*color+B)+D*F))-E/F;
}

float4 tonemap(float4 color, TonemapParameters params)
{
	float3 outcol = Uncharted2Tonemap(color.rgb * params.exposure);
	outcol = outcol * (1.0f / Uncharted2Tonemap(float3(11.2f)));	
	return float4(pow(outcol, float3(1.0f / params.gamma)), color.a);
}