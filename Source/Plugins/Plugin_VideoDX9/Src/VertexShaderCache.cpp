// Copyright (C) 2003-2008 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include <map>

#include "D3DBase.h"
#include "D3DShader.h"
#include "Statistics.h"
#include "Utils.h"
#include "Profiler.h"
#include "VertexShaderCache.h"
#include "VertexLoader.h"
#include "BPMemory.h"
#include "XFMemory.h"

#include <Cg/cg.h>
#include <Cg/cgD3D9.h>

VertexShaderCache::VSCache VertexShaderCache::vshaders;

void SetVSConstant4f(int const_number, float f1, float f2, float f3, float f4)
{
	const float f[4] = {f1, f2, f3, f4};
	D3D::dev->SetVertexShaderConstantF(const_number, f, 1);
}

void SetVSConstant4fv(int const_number, const float *f)
{
	D3D::dev->SetVertexShaderConstantF(const_number, f, 1);
}


void VertexShaderCache::Init()
{

}


void VertexShaderCache::Shutdown()
{
	VSCache::iterator iter = vshaders.begin();
	for (; iter != vshaders.end(); iter++)
		iter->second.Destroy();
	vshaders.clear();
}


void VertexShaderCache::SetShader(u32 components)
{
	if (D3D::GetShaderVersion() < 2)
		return; // we are screwed

	static LPDIRECT3DVERTEXSHADER9 lastShader = NULL;

	DVSTARTPROFILE();

	VERTEXSHADERUID uid;
	GetVertexShaderId(uid, components, false);

	VSCache::iterator iter;
	iter = vshaders.find(uid);
	if (iter != vshaders.end())
	{
		iter->second.frameCount = frameCount;
		VSCacheEntry &entry = iter->second;
		if (!lastShader || entry.shader != lastShader)
		{
			D3D::dev->SetVertexShader(entry.shader);
			lastShader = entry.shader;
		}
		return;
	}

	const char *code = GenerateVertexShader(components, false);
	LPDIRECT3DVERTEXSHADER9 shader = CompileCgShader(code);
	if (shader)
	{
		// Make an entry in the table
		VSCacheEntry entry;
		entry.shader = shader;
		entry.frameCount = frameCount;
		vshaders[uid] = entry;

		D3D::dev->SetVertexShader(shader);

		INCSTAT(stats.numVertexShadersCreated);
		SETSTAT(stats.numVertexShadersAlive, (int)vshaders.size());
	} else {
		PanicAlert("Failed to compile Vertex Shader:\n\n%s", code);
	}

	D3D::dev->SetFVF(NULL);
	D3D::dev->SetVertexShader(shader);
}

LPDIRECT3DVERTEXSHADER9 VertexShaderCache::CompileCgShader(const char *pstrprogram) 
{
	//char stropt[64];
	//sprintf(stropt, "MaxLocalParams=256,MaxInstructions=%d", s_nMaxVertexInstructions);
	const char *opts[] = {"-profileopts", "MaxLocalParams=256", "-O2", "-q", NULL};
	//const char **opts = cgD3D9GetOptimalOptions(g_cgvProf);
	CGprogram tempprog = cgCreateProgram(g_cgcontext, CG_SOURCE, pstrprogram, g_cgvProf, "main", opts);
	if (!cgIsProgram(tempprog) || cgGetError() != CG_NO_ERROR) {
		ERROR_LOG(VIDEO, "Failed to load vs %s:\n", cgGetLastListing(g_cgcontext));
		ERROR_LOG(VIDEO, pstrprogram);
		return NULL;
	}
	const char *pcompiledprog = cgGetProgramString(tempprog, CG_COMPILED_PROGRAM);

	LPD3DXBUFFER shader_binary;
	LPD3DXBUFFER error_msg;

	// Step one - Assemble into binary code. This binary code could be cached.
	if (FAILED(D3DXAssembleShader(pcompiledprog, (UINT)strlen(pcompiledprog), NULL, NULL, 0, &shader_binary, &error_msg)))
		PanicAlert("Asm fail");
	// Destroy Cg program as early as possible - we want as little as possible to do with Cg due to
	// our rather extreme performance requirements.
	cgDestroyProgram(tempprog);
	tempprog = NULL;

	// Create vertex shader from the binary code.
	LPDIRECT3DVERTEXSHADER9 vertex_shader = NULL;
	if (SUCCEEDED(D3D::dev->CreateVertexShader((const DWORD *)shader_binary->GetBufferPointer(), &vertex_shader))) {
		// PanicAlert("Successvertex!");
	} else {
		if (error_msg) {
			PanicAlert("failure vertex %s", error_msg->GetBufferPointer());
			MessageBox(0, pcompiledprog, 0, 0);
		}
		else
			PanicAlert("failure vertex with no error message.");
	}
	if (shader_binary)
		shader_binary->Release();
	if (error_msg)
		error_msg->Release();
	return vertex_shader;
}

void VertexShaderCache::Cleanup()
{
	for (VSCache::iterator iter = vshaders.begin(); iter != vshaders.end();)
	{
		VSCacheEntry &entry = iter->second;
		if (entry.frameCount < frameCount - 30)
		{
			entry.Destroy();
			iter = vshaders.erase(iter);
		}
		else
		{
			++iter;
		}
	}
	SETSTAT(stats.numVertexShadersAlive, (int)vshaders.size());
}