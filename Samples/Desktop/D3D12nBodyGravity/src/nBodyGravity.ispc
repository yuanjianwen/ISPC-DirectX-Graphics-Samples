//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2017, Intel Corporation
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of 
// the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
// SOFTWARE.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////// 

struct Vec4
{
    float x;
    float y;
    float z;
    float w;
};

struct Vec3
{
    float x;
    float y;
    float z;
};

struct Particle
{
    Vec4 position;
    Vec4 velocity;
}; 

//
// Use the fast reciprocal sqrt from
// https://en.wikipedia.org/wiki/Fast_inverse_square_root
//
inline float Q_rsqrt(float number)
{
    int i;
    float x2, y;
    const float threehalfs = 1.5F;

    x2 = number * 0.5f;
    y = number;
    i = intbits(y);                       // evil floating point bit level hacking
    i = 0x5f3759df - (i >> 1);
    y = floatbits(i);
    y = y * (threehalfs - (x2 * y * y));   // 1st iteration
    // y  = y * ( threehalfs - ( x2 * y * y ) );   // 2nd iteration, this can be removed

    return y;
}

inline void bodyBodyInteraction(
    Vec3 &accel,
    uniform Vec4 thatPos,
    Vec3 thisPos)
{
    const float softeningSquared = 0.0000015625f;
    const float g_fParticleMass = 66.73f;

    Vec3 r;
    r.x = thatPos.x - thisPos.x;
    r.y = thatPos.y - thisPos.y;
    r.z = thatPos.z - thisPos.z;

    float distSqr = (r.x * r.x) + (r.y * r.y) + (r.z * r.z);
    distSqr += softeningSquared;

    float invDist = Q_rsqrt(distSqr);
    float invDistCube = invDist * invDist * invDist;

    float s = g_fParticleMass * invDistCube;

    accel.x += r.x * s;
    accel.y += r.y * s;
    accel.z += r.z * s;
}

export void ProcessParticles(uniform unsigned int particleStart, uniform unsigned int particleCount, uniform Particle readParticles[], uniform Particle writeParticles[], uniform unsigned int totalParticles)
{
    const float timeStepDelta = 0.1f;

    uniform unsigned int particleEnd = particleStart + particleCount;

    //
    // Vectorise the outer loop so we will work on N particles at once in an ISPC gang (SIMD vector of width N). 
    //
    //	SSE4 will provide a gang width of 4
    //	AVX2 will provide a gang width of 8
    //
    // The particle data is in AoS format, so requires gathers/scatter to load/store the data. This can
    // be expensive in SIMD, but that cost is amortized as we do so much work for each particle. AVX2 offers
    // good performance gains through its gather/broadcast instructions for this.
    //
    foreach(ii = particleStart ... particleEnd)
    {
        //
        // pos and accel are varyings, so they are vectorised across the gang
        //
        Vec3 accel = { 0.0f, 0.0f, 0.0f};
        Vec3 pos;

        //
        // This requires scatter/gathers and ISPC will produce a warning :
        //		nBodyGravity.ispc:116:21: Performance Warning: Gather required to load value.
        //			  pos.x = readParticles[ii].position.x;
        //				      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        //
        pos.x = readParticles[ii].position.x;
        pos.y = readParticles[ii].position.y;
        pos.z = readParticles[ii].position.z;

        //
        // The inner loop remains scalar, so each element of the position is loaded into a scalar register
        // and shared amongst the gang of particles from the outer loop.
        //
        // The loop unrolling provides good performance gains
        //
        for (uniform unsigned int jj = 0; jj < totalParticles; jj += 8)
        {
            //
            // The read particles are scalar, so each lane of the gang/vector works on the same readparticle
            //
            bodyBodyInteraction(accel, readParticles[jj + 0].position, pos);
            bodyBodyInteraction(accel, readParticles[jj + 1].position, pos);
            bodyBodyInteraction(accel, readParticles[jj + 2].position, pos);
            bodyBodyInteraction(accel, readParticles[jj + 3].position, pos);
            bodyBodyInteraction(accel, readParticles[jj + 4].position, pos);
            bodyBodyInteraction(accel, readParticles[jj + 5].position, pos);
            bodyBodyInteraction(accel, readParticles[jj + 6].position, pos);
            bodyBodyInteraction(accel, readParticles[jj + 7].position, pos);
        }

        //
        // Update the velocity and position of current particle using the 
        // acceleration computed above.
        //
        Vec4 vel = readParticles[ii].velocity;

        vel.x += accel.x * timeStepDelta;
        vel.y += accel.y * timeStepDelta;
        vel.z += accel.z * timeStepDelta;
        vel.w = 1.0f / Q_rsqrt((accel.x * accel.x) + (accel.y * accel.y) + (accel.z * accel.z));

        pos.x += vel.x * timeStepDelta;
        pos.y += vel.y * timeStepDelta;
        pos.z += vel.z * timeStepDelta;

        //
        // Store the newly computed particle data
        // This requires scatters and ISPC will produce a warning :
        //		nBodyGravity.ispc:158:9: Performance Warning: Scatter required to store value.
        //			writeParticles[ii].position.x = pos.x;
        //			^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        //
        writeParticles[ii].position.x = pos.x;
        writeParticles[ii].position.y = pos.y;
        writeParticles[ii].position.z = pos.z;
        writeParticles[ii].velocity = vel;
    }
}







