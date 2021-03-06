//========================================================================
// Copyright (c) Yann Clotioloman Yeo, 2018
//
//	Author					: Yann Clotioloman Yeo
//	E-Mail					: nebularender@gmail.com
//========================================================================

#include "stdafx.h"

#include "CameraConstantBuffer.h"
#include "Graphics/Light/DirectionnalLight.h"
#include "Graphics/Light/OmniLight.h"
#include "Graphics/Material/DefaultDielectric.h"
#include "Graphics/Material/DefaultMetal.h"
#include "Graphics/Material/Hair.h"
#include "Graphics/Renderer/Realtime/Dx12/Dx12Renderer.h"
#include "ForwardLighning.h"
#include "MeshGroupConstantBuffer.h"
#include <dxgi1_4.h>
#include <minwinbase.h>

namespace Graphics { namespace Renderer { namespace Realtime { namespace Dx12 { namespace Effect
{
using namespace DirectX;
using namespace Entity;

ForwardLighning::ForwardLighning(const DXGI_SAMPLE_DESC& sampleDesc)
: BaseEffect(sampleDesc)
, m_pixelShaderMaterialCBUploadHeap(nullptr)
, m_pixelShaderMaterialCBDefaultHeap(nullptr)
{
	initRootSignature();
	initPipelineStateObjects();
	initStaticConstantBuffers();
}

ForwardLighning::~ForwardLighning()
{
	NEBULA_DX12_SAFE_RELEASE(m_pixelShaderMaterialCBUploadHeap);
	NEBULA_DX12_SAFE_RELEASE(m_pixelShaderMaterialCBDefaultHeap);
}

void ForwardLighning::onUpdateMaterial(const Scene::BaseScene& scene, const EntityIdentifier& matId, ID3D12GraphicsCommandList* commandList)
{
	commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_pixelShaderMaterialCBDefaultHeap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));

	updateMaterial(scene, matId, commandList);

	fromMaterialUploadToDefaultHeaps(commandList);
}

void ForwardLighning::initRootSignature()
{
	D3D12_ROOT_PARAMETER rootParameters[7];

	// A root descriptor, which explains where to find the data for the parameter
	D3D12_ROOT_DESCRIPTOR rootCBVDescriptor;
	rootCBVDescriptor.RegisterSpace = 0;

	// 0 & 1 : Root parameter for the vertex shader shared constant buffers
	nbInt32 paramIdx = 0;
	for (; paramIdx < 2; ++paramIdx)
	{
		rootCBVDescriptor.ShaderRegister = paramIdx;
		rootParameters[paramIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[paramIdx].Descriptor = rootCBVDescriptor;
		rootParameters[paramIdx].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	}

	// 2 & 3 : Root parameter for the light and material pixel shader constant buffer
	for (nbInt32 i = 0; i < 2; ++i, ++paramIdx)
	{
		rootCBVDescriptor.ShaderRegister = i;
		rootParameters[paramIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[paramIdx].Descriptor = rootCBVDescriptor;
		rootParameters[paramIdx].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}

	// 4 & 5 & 6 Root parameter for the pixel shader textures.
	// create a descriptor range and fill it out
	// this is a range of descriptors inside a descriptor heap
	static const nbInt32 nbTextures = 3;
	D3D12_DESCRIPTOR_RANGE descriptorTableRanges[nbTextures];

	for (nbInt32 i = 0; i < nbTextures; ++i, ++paramIdx)
	{
		descriptorTableRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // this is a range of shader resource views (descriptors)
		descriptorTableRanges[i].NumDescriptors = 1;
		descriptorTableRanges[i].RegisterSpace = 0; // space 0. can usually be zero
		descriptorTableRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND; // this appends the range to the end of the root signature descriptor tables
		descriptorTableRanges[i].BaseShaderRegister = i; // start index of the shader registers in the range
		
		// Create a descriptor table
		D3D12_ROOT_DESCRIPTOR_TABLE descriptorTable;
		descriptorTable.NumDescriptorRanges = 1;
		descriptorTable.pDescriptorRanges = &descriptorTableRanges[i];
		rootParameters[paramIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[paramIdx].DescriptorTable = descriptorTable;
		rootParameters[paramIdx].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}

	// Create root signature
	createRootSignature(rootParameters, _countof(rootParameters));
}

void ForwardLighning::initPipelineStateObjects()
{
	// Create input layout
	D3D12_INPUT_ELEMENT_DESC inputLayoutElement[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	// The blending description
	D3D12_BLEND_DESC blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Depth stencil description
	D3D12_DEPTH_STENCIL_DESC dsDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	dsDesc.DepthEnable = TRUE;
	dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

	dsDesc.StencilEnable = TRUE;
	dsDesc.StencilReadMask = 0xFF;
	dsDesc.StencilWriteMask = 0xFF;

	// Front-facing pixels.
	dsDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

	// Back-facing pixels.
	dsDesc.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

	// Compile vertex shader
	ID3DBlob* vertexShader = compileShader(std::wstring(L"ForwardLightning_VS.hlsl"), true);

	// Pixel shader macros
	auto maxLightDef = std::to_string(Graphics::Light::MaxLightsPerScene);
	auto noFresnelValueDef = std::to_string(NEBULA_NO_FRESNEL_VALUE);

	auto defaultDielectricIdxDef = std::to_string(Material::BaseMaterial::Type::DefaultDielectric);
	auto defaultMetalIdxDef = std::to_string(Material::BaseMaterial::Type::DefaultMetal);
	auto perfectMirrorIdxDef = std::to_string(Material::BaseMaterial::Type::PerfectMirror);
	auto plasticIdxDef = std::to_string(Material::BaseMaterial::Type::Plastic);
	auto hairIdxDef = std::to_string(Material::BaseMaterial::Type::Hair);
	auto sssIdxDef = std::to_string(Material::BaseMaterial::Type::SSS);
	
	D3D_SHADER_MACRO macros[] = {	"MAX_LIGHTS", maxLightDef.c_str(),
									"NO_FRESNEL_VALUE", noFresnelValueDef.c_str(),
									"DEFAULT_DIELECTRIC_IDX", defaultDielectricIdxDef.c_str(),
									"DEFAULT_METAL_IDX", defaultMetalIdxDef.c_str(),
									"PERFECT_MIRROR_IDX", perfectMirrorIdxDef.c_str(),
									"PLASTIC_IDX", plasticIdxDef.c_str(),
									"HAIR_IDX", hairIdxDef.c_str(),
									"SSS_IDX", sssIdxDef.c_str(), NULL, NULL
	};

	// Compile pixel shader
	ID3DBlob* pixelShader = compileShader(std::wstring(L"ForwardLightning_PS.hlsl"), false, macros);

	// Compile solid PSO
	compilePipeline(m_solidPSO,
		vertexShader,
		pixelShader,
		inputLayoutElement,
		sizeof(inputLayoutElement) / sizeof(D3D12_INPUT_ELEMENT_DESC),
		blendDesc,
		dsDesc,
		CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT)
	);
	
	m_solidPSO->SetName(L"Forward lightning solid PSO");

	// Compile wireframe PSO
	const CD3DX12_RASTERIZER_DESC rasterizerDesc(D3D12_FILL_MODE_WIREFRAME, D3D12_CULL_MODE_NONE,
		FALSE /* FrontCounterClockwise */,
		D3D12_DEFAULT_DEPTH_BIAS,
		D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
		D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
		TRUE /* DepthClipEnable */,
		TRUE /* MultisampleEnable */,
		FALSE /* AntialiasedLineEnable */,
		0 /* ForceSampleCount */,
		D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF);

	compilePipeline(m_wireframePSO,
		vertexShader,
		pixelShader,
		inputLayoutElement,
		sizeof(inputLayoutElement) / sizeof(D3D12_INPUT_ELEMENT_DESC),
		blendDesc,
		dsDesc,
		rasterizerDesc
	);

	m_wireframePSO->SetName(L"Forward lightning wireframe PSO");
}

void ForwardLighning::initStaticConstantBuffers()
{
	MAKE_SWAP_CHAIN_ITERATOR_I
	{
		HRESULT hr = D3D12Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(PixelShaderLightCBAlignedSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_pixelShaderLightsCBUploadHeaps[i]));

		NEBULA_ASSERT(SUCCEEDED(hr));
		m_pixelShaderLightsCBUploadHeaps[i]->SetName(L"Pixel shader lights constant buffer upload heap");

		hr = m_pixelShaderLightsCBUploadHeaps[i]->Map(0, &ReadRangeGPUOnly, reinterpret_cast<void**>(&m_pixelShaderLightsCBGPUAddress[i]));
		NEBULA_ASSERT(SUCCEEDED(hr));
	}
}

void ForwardLighning::initDynamicMaterialConstantBuffer(const Scene::BaseScene& scene, ID3D12GraphicsCommandList* commandList)
{
	const auto& materials = scene.getModel()->getMaterials();
	if (materials.empty())
		return;

	// Allocate the maximal possible number of materials.
	// This allow material references to be still be valid during deletion.
	const nbUint32 bufferSize = (nbUint32)materials.size();

	if (bufferSize > m_materialBufferSize)
	{
		m_materialBufferSize = bufferSize;

		// 0 : Init heaps
		NEBULA_DX12_SAFE_RELEASE(m_pixelShaderMaterialCBUploadHeap);
		NEBULA_DX12_SAFE_RELEASE(m_pixelShaderMaterialCBDefaultHeap);

		// Create upload heap
		HRESULT hr = D3D12Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(PixelShaderMaterialCBAlignedSize * bufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_pixelShaderMaterialCBUploadHeap));

		NEBULA_ASSERT(SUCCEEDED(hr));
		m_pixelShaderMaterialCBUploadHeap->SetName(L"Pixel shader material Constant Buffer Upload heap");

		hr = m_pixelShaderMaterialCBUploadHeap->Map(0, &ReadRangeGPUOnly, reinterpret_cast<void**>(&m_pixelShaderMaterialCBGPUAddress));
		NEBULA_ASSERT(SUCCEEDED(hr));

		// Create default heap
		hr = D3D12Device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(PixelShaderMaterialCBAlignedSize * bufferSize),
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&m_pixelShaderMaterialCBDefaultHeap));

		NEBULA_ASSERT(SUCCEEDED(hr));
		m_pixelShaderMaterialCBDefaultHeap->SetName(L"Pixel shader material Constant Buffer Default Resource heap");
	}

	// 1 : Update materials in upload heap
	m_materialCBPositions.clear();
	for (const auto& mat : materials)
	{
		m_materialCBPositions.emplace(mat, (nbUint32)m_materialCBPositions.size() * PixelShaderMaterialCBAlignedSize);

		updateMaterial(scene, mat, commandList);
	}

	// 2: Copy to default heap
	fromMaterialUploadToDefaultHeaps(commandList);
}

void ForwardLighning::fromMaterialUploadToDefaultHeaps(ID3D12GraphicsCommandList* commandList)
{
	// Copy upload heap to default
	commandList->CopyResource(m_pixelShaderMaterialCBDefaultHeap, m_pixelShaderMaterialCBUploadHeap);

	// Transition to pixel shader resource.
	commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_pixelShaderMaterialCBDefaultHeap, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void ForwardLighning::updateMaterial(const Scene::BaseScene& scene, const EntityIdentifier& matId, ID3D12GraphicsCommandList* commandList)
{
	using namespace Material;

	const DX12Model* dx12Model = static_cast<const DX12Model*>(scene.getModel().get());
	auto materialHandle = dx12Model->getMaterialHandle(matId);

	PixelShaderMaterialCB pixelShaderMaterialCB;
	auto& materialCB = pixelShaderMaterialCB.material;
	ZeroMemory(&pixelShaderMaterialCB, sizeof(pixelShaderMaterialCB));

	const BaseMaterial* material = dx12Model->getMaterialFromEntityOrDefault(materialHandle->matId).get();
	if (material->isFresnelMaterial())
	{
		const FresnelMaterial* fresnelMat = static_cast<const FresnelMaterial*>(material);

		materialCB.ambient = { fresnelMat->getAmbient().r, fresnelMat->getAmbient().g, fresnelMat->getAmbient().b, 1.0f };
		materialCB.shininess = fresnelMat->getShininess();
		materialCB.roughness = fresnelMat->getRoughness();
		materialCB.fresnel0 = fresnelMat->getFresnel0();
		materialCB.fresnelEnabled = (BOOL)fresnelMat->getFresnelEnabled();
	}
	else
	{
		materialCB.diffuse = { defaultAmbient, defaultAmbient, defaultAmbient, 1.0f };
		materialCB.ambient = { defaultAmbient, defaultAmbient, defaultAmbient, 1.0f };
	}

	if (material->isDielectric())
	{
		const DefaultDielectric* dielectricMat = static_cast<const DefaultDielectric*>(material);
		materialCB.diffuse = { dielectricMat->getDiffuse().r, dielectricMat->getDiffuse().g, dielectricMat->getDiffuse().b, 1.0f };
		materialCB.specular = { dielectricMat->getSpecular().r, dielectricMat->getSpecular().g, dielectricMat->getSpecular().b, 1.0f };
		materialCB.emissive = { dielectricMat->getEmissive().r, dielectricMat->getEmissive().g, dielectricMat->getEmissive().b, 1.0f };
	}
	else if (material->getType() == BaseMaterial::Type::DefaultMetal)
	{
		const DefaultMetal* metalMat = static_cast<const DefaultMetal*>(material);
		materialCB.diffuse = { metalMat->getReflectance().r, metalMat->getReflectance().g, metalMat->getReflectance().b, 1.0f };
	}
	else if (material->getType() == BaseMaterial::Type::Hair)
	{
		const Hair* hairMat = static_cast<const Hair*>(material);
		materialCB.diffuse = { hairMat->getReflectance().r, hairMat->getReflectance().g, hairMat->getReflectance().b, 1.0f };
	}

	materialCB.opacity = material->getOpacity();
	materialCB.type = static_cast<INT>(material->getType());

	materialCB.hasDiffuseTex = static_cast<INT>(dx12Model->getTextureHandle(materialHandle->diffuseTexture) != nullptr);
	materialCB.hasSpecularTex = static_cast<INT>(dx12Model->getTextureHandle(materialHandle->specularTexture) != nullptr);
	materialCB.hasNormalTex = static_cast<INT>(dx12Model->getTextureHandle(materialHandle->normalTexture) != nullptr);

	memcpy(m_pixelShaderMaterialCBGPUAddress + m_materialCBPositions[matId], &pixelShaderMaterialCB, sizeof(PixelShaderMaterialCB));
}

void ForwardLighning::updatePixelShaderLightsCB(ForwardLightningPushArgs& data, nbInt32 frameIndex)
{
	ZeroMemory(&m_pixelShaderLightsCB, sizeof(PixelShaderEnvironmentCb));

	if (const auto& media = data.scene.getMedia())
	{
		m_pixelShaderLightsCB.mediaInfo = {
			1.0f,
			0.5f,
			media->getExtinctionCoeff(),
			1.0f
		};
	}

	const auto& lights = data.scene.getLights();

	const nbUint32 lightSize = static_cast<nbUint32>(lights.size());
	m_pixelShaderLightsCB.nbLights = lightSize;

	const glm::vec3 cameraPos = data.scene.getCamera().get()->getPosition();
	m_pixelShaderLightsCB.eyePosition = { cameraPos.x, cameraPos.y, cameraPos.z, 0.0f};

	const auto& ambientColor = data.scene.getAmbientColor();
	m_pixelShaderLightsCB.sceneAmbient = { ambientColor.x, ambientColor.y, ambientColor.z, 0.0f};

	nbInt32 lightIdx = 0;

	for (const auto& lightId : lights)
	{
		auto& dx12Light = m_pixelShaderLightsCB.lights[lightIdx];
		const auto light = Light::getLightFromEntity(lightId);

		const auto lightType = light->getType();

		dx12Light.type = static_cast<nbInt32>(lightType);

		if (light->getType() == Light::LightType::Point)
		{
			auto* omniLight = static_cast<Light::OmniLight*>(light.get());

			const auto pos = omniLight->getPosition();
			dx12Light.position = { pos.x, pos.y, pos.z, 0.0f };
			dx12Light.range = omniLight->getRange();
		}
		else if (light->getType() == Graphics::Light::LightType::Directionnal)
		{
			auto* directionnalLight = static_cast<Graphics::Light::DirectionnalLight*>(light.get());
			const auto& direction = directionnalLight->getDirection();

			dx12Light.direction = { direction.x, direction.y, direction.z, 0.0f };
		}

		const auto& color = light->getFinalColor();
		dx12Light.color = { color.r, color.g, color.r, 0.0f };

		++lightIdx;
	}

	memcpy(m_pixelShaderLightsCBGPUAddress[frameIndex], &m_pixelShaderLightsCB, sizeof(PixelShaderEnvironmentCb));
}

void ForwardLighning::pushDrawCommands(ForwardLightningPushArgs& data, ID3D12GraphicsCommandList* commandList, nbInt32 frameIndex)
{
	// Set Pso
	commandList->SetPipelineState(data.scene.isWireframeEnabled() ? m_wireframePSO : m_solidPSO);

	// Set root signature
	commandList->SetGraphicsRootSignature(m_rootSignature);

	// Set the primitive topology
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Update light constant buffers
	updatePixelShaderLightsCB(data, frameIndex);

	// Set shared constant buffer views
	commandList->SetGraphicsRootConstantBufferView(0, CameraConstantBufferSingleton::instance()->getUploadtHeaps()[frameIndex]->GetGPUVirtualAddress());
	commandList->SetGraphicsRootConstantBufferView(2, m_pixelShaderLightsCBUploadHeaps[frameIndex]->GetGPUVirtualAddress());

	const auto* dx12Model = static_cast<const DX12Model*>(data.scene.getModel().get());
	const auto& meshGroups = dx12Model->getMeshGroups();

	auto& getPSReadyTextureHandle = [dx12Model, commandList](const EntityIdentifier& imageId, const DX12Model::SharedTMaterialHandlePtr& materialHandle, nbInt32 rootParamIdx)
	{
		const auto tex = imageId ? dx12Model->getTextureHandle(imageId) : nullptr;
		if (tex)
		{
			commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(tex->getHandle().buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
			commandList->SetGraphicsRootDescriptorTable(rootParamIdx, tex->getHandle().descriptorHandle.getGpuHandle());
		}

		return tex;
	};

	for (const auto& group : dx12Model->getMeshHandlesByGroup())
	{
		const auto meshByGroupPtr = Model::getMeshGroupFromEntity(group.first);
		if (!meshByGroupPtr->m_enabled)
			continue;

		commandList->SetGraphicsRootConstantBufferView(1, MeshGroupConstantBufferSingleton::instance()->getMeshGroupGPUVirtualAddress(group.first));

		const auto materialHandle = dx12Model->getMaterialHandle(meshByGroupPtr->m_materialId);
		commandList->SetGraphicsRootConstantBufferView(3, m_pixelShaderMaterialCBDefaultHeap->GetGPUVirtualAddress() + m_materialCBPositions[meshByGroupPtr->m_materialId]);
		
		// Set textures states.
		const auto diffuseTex = getPSReadyTextureHandle(materialHandle->diffuseTexture, materialHandle, 4);
		const auto specularTex = getPSReadyTextureHandle(materialHandle->specularTexture, materialHandle, 5);
		const auto normalTex = getPSReadyTextureHandle(materialHandle->normalTexture, materialHandle, 6);

		// Draw meshes.
		for (auto* meshHandle : group.second)
		{
			commandList->IASetVertexBuffers(0, 1, &meshHandle->vertexBuffer.bufferView);
			commandList->IASetIndexBuffer(&meshHandle->indexBuffer.bufferView);

			commandList->DrawIndexedInstanced(meshHandle->nbIndices, 1, 0, 0, 0);
		}

		// Reset texture states.
		if (diffuseTex)
			commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(diffuseTex->getHandle().buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		
		if (specularTex)
			commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(specularTex->getHandle().buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
	
		if (normalTex)
			commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalTex->getHandle().buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
	}
}
}}}}}
