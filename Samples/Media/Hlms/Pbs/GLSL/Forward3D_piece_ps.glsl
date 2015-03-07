@property( hlms_forward3d )
@piece( forward3dLighting )
	float f3dMinDistance	= pass.f3dData.x;
	float f3dInvMaxDistance	= pass.f3dData.y;
	float f3dNumSlicesSub1	= pass.f3dData.z;
	uint cellsPerTableOnGrid0= floatBitsToUint( pass.f3dData.w );

	// See C++'s Forward3D::getSliceAtDepth
	/*float fSlice = 1.0 - clamp( (-inPs.pos.z + f3dMinDistance) * f3dInvMaxDistance, 0.0, 1.0 );
	fSlice = (fSlice * fSlice) * (fSlice * fSlice);
	fSlice = (fSlice * fSlice);
	fSlice = floor( (1.0 - fSlice) * f3dNumSlicesSub1 );*/
	float fSlice = clamp( (-inPs.pos.z + f3dMinDistance) * f3dInvMaxDistance, 0.0, 1.0 );
	fSlice = floor( fSlice * f3dNumSlicesSub1 );
	uint slice = uint( fSlice );

	//TODO: Profile performance: derive this mathematically or use a lookup table?
	uint offset = cellsPerTableOnGrid0 * (((1u << (slice << 1u)) - 1u) / 3u);

	float lightsPerCell = pass.f3dGridHWW[0].w;

	//pass.f3dGridHWW[slice].x = grid_width / renderTarget->width;
	//pass.f3dGridHWW[slice].y = grid_height / renderTarget->height;
	//pass.f3dGridHWW[slice].z = grid_width * lightsPerCell;
	//uint sampleOffset = 0;
	uint sampleOffset = offset +
						uint(floor( gl_FragCoord.y * pass.f3dGridHWW[slice].y ) * pass.f3dGridHWW[slice].z) +
						uint(floor( gl_FragCoord.x * pass.f3dGridHWW[slice].x ) * lightsPerCell);

	uint numLightsInGrid = texelFetch( f3dGrid, int(sampleOffset) ).x;

	for( uint i=0; i<numLightsInGrid; ++i )
	{
		//Get the light index
		uint idx = texelFetch( f3dGrid, int(sampleOffset + i + 1) ).x;

		//Get the light
		vec4 posAndType = texelFetch( f3dLightList, int(idx) );

		vec3 lightDiffuse	= texelFetch( f3dLightList, int(idx + 1) ).xyz;
		vec3 lightSpecular	= texelFetch( f3dLightList, int(idx + 2) ).xyz;
		vec3 attenuation	= texelFetch( f3dLightList, int(idx + 3) ).xyz;

		vec3 lightDir	= posAndType.xyz - inPs.pos;
		float fDistance	= length( lightDir );

		if( fDistance <= attenuation.x )
		{
			lightDir *= 1.0 / fDistance;
			float atten = 1.0 / (1.0 + (attenuation.y + attenuation.z * fDistance) * fDistance );

			if( posAndType.w == 1.0 )
			{
				//Point light
				vec3 tmpColour = cookTorrance( lightDir, viewDir, NdotV, lightDiffuse, lightSpecular );
				finalColour += tmpColour * atten;
			}
			else
			{
				//spotParams.x = 1.0 / cos( InnerAngle ) - cos( OuterAngle )
				//spotParams.y = cos( OuterAngle / 2 )
				//spotParams.z = falloff

				//Spot light
				vec3 spotDirection	= texelFetch( f3dLightList, int(idx + 4) ).xyz;
				vec3 spotParams		= texelFetch( f3dLightList, int(idx + 5) ).xyz;

				float spotCosAngle = dot( normalize( inPs.pos - posAndType.xyz ), spotDirection.xyz );

				float spotAtten = clamp( (spotCosAngle - spotParams.y) * spotParams.x, 0.0, 1.0 );
				spotAtten = pow( spotAtten, spotParams.z );
				atten *= spotAtten;

				if( spotCosAngle >= spotParams.y )
				{
					vec3 tmpColour = cookTorrance( lightDir, viewDir, NdotV, lightDiffuse, lightSpecular );
					finalColour += tmpColour * atten;
				}
			}
		}
	}

	@property( hlms_forward3d_debug )
		float occupancy = (numLightsInGrid / pass.f3dGridHWW[0].w);
		vec3 occupCol = vec3( 0.0, 0.0, 0.0 );
		if( occupancy < 1.0 / 3.0 )
			occupCol.z = occupancy;
		else if( occupancy < 2.0 / 3.0 )
			occupCol.y = occupancy;
		else
			occupCol.x = occupancy;

		finalColour.xyz = mix( finalColour.xyz, occupCol.xyz, 0.95f );
	@end
@end
@end