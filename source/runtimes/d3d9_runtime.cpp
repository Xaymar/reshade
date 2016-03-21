#include "log.hpp"
#include "d3d9_runtime.hpp"
#include "d3d9_fx_compiler.hpp"
#include "gui.hpp"
#include "input.hpp"

#include <assert.h>
#include <nanovg_d3d9.h>

const D3DFORMAT D3DFMT_INTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

namespace reshade
{
	d3d9_runtime::d3d9_runtime(IDirect3DDevice9 *device, IDirect3DSwapChain9 *swapchain) : runtime(0x9300), _device(device), _swapchain(swapchain), _is_multisampling_enabled(false), _backbuffer_format(D3DFMT_UNKNOWN), _constant_register_count(0)
	{
		assert(device != nullptr);
		assert(swapchain != nullptr);

		_device->GetDirect3D(&_d3d);

		assert(_d3d != nullptr);

		D3DCAPS9 caps;
		D3DADAPTER_IDENTIFIER9 adapter_desc;
		D3DDEVICE_CREATION_PARAMETERS creation_params;

		_device->GetDeviceCaps(&caps);
		_device->GetCreationParameters(&creation_params);
		_d3d->GetAdapterIdentifier(creation_params.AdapterOrdinal, 0, &adapter_desc);

		_vendor_id = adapter_desc.VendorId;
		_device_id = adapter_desc.DeviceId;
		_behavior_flags = creation_params.BehaviorFlags;
		_num_simultaneous_rendertargets = std::min(caps.NumSimultaneousRTs, 8ul);
	}

	bool d3d9_runtime::on_init(const D3DPRESENT_PARAMETERS &pp)
	{
		_width = pp.BackBufferWidth;
		_height = pp.BackBufferHeight;
		_backbuffer_format = pp.BackBufferFormat;
		_is_multisampling_enabled = pp.MultiSampleType != D3DMULTISAMPLE_NONE;
		input::register_window(pp.hDeviceWindow, _input);

		// Get back buffer surface
		HRESULT hr = _swapchain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &_backbuffer);

		assert(SUCCEEDED(hr));

		if (pp.MultiSampleType != D3DMULTISAMPLE_NONE || (pp.BackBufferFormat == D3DFMT_X8R8G8B8 || pp.BackBufferFormat == D3DFMT_X8B8G8R8))
		{
			switch (pp.BackBufferFormat)
			{
				case D3DFMT_X8R8G8B8:
					_backbuffer_format = D3DFMT_A8R8G8B8;
					break;
				case D3DFMT_X8B8G8R8:
					_backbuffer_format = D3DFMT_A8B8G8R8;
					break;
			}

			hr = _device->CreateRenderTarget(_width, _height, _backbuffer_format, D3DMULTISAMPLE_NONE, 0, FALSE, &_backbuffer_resolved, nullptr);

			if (FAILED(hr))
			{
				LOG(TRACE) << "Failed to create back buffer resolve texture! HRESULT is '" << std::hex << hr << std::dec << "'.";

				return false;
			}
		}
		else
		{
			_backbuffer_resolved = _backbuffer;
		}

		// Create back buffer shader texture
		hr = _device->CreateTexture(_width, _height, 1, D3DUSAGE_RENDERTARGET, _backbuffer_format, D3DPOOL_DEFAULT, &_backbuffer_texture, nullptr);

		if (SUCCEEDED(hr))
		{
			_backbuffer_texture->GetSurfaceLevel(0, &_backbuffer_texture_surface);
		}
		else
		{
			LOG(TRACE) << "Failed to create back buffer texture! HRESULT is '" << std::hex << hr << std::dec << "'.";

			return false;
		}

		// Create default depth-stencil surface
		hr = _device->CreateDepthStencilSurface(_width, _height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &_default_depthstencil, nullptr);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create default depth-stencil! HRESULT is '" << std::hex << hr << std::dec << "'.";

			return false;
		}

		// Create effect state block and objects
		hr = _device->CreateStateBlock(D3DSBT_ALL, &_stateblock);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create state block! HRESULT is '" << std::hex << hr << std::dec << "'.";

			return false;
		}

		hr = _device->CreateVertexBuffer(3 * sizeof(float), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &_effect_triangle_buffer, nullptr);

		if (SUCCEEDED(hr))
		{
			float *data = nullptr;

			hr = _effect_triangle_buffer->Lock(0, 3 * sizeof(float), reinterpret_cast<void **>(&data), 0);

			assert(SUCCEEDED(hr));

			for (UINT i = 0; i < 3; i++)
			{
				data[i] = static_cast<float>(i);
			}

			_effect_triangle_buffer->Unlock();
		}
		else
		{
			LOG(TRACE) << "Failed to create effect vertex buffer! HRESULT is '" << std::hex << hr << std::dec << "'.";

			return false;
		}

		const D3DVERTEXELEMENT9 declaration[] =
		{
			{ 0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			D3DDECL_END()
		};

		hr = _device->CreateVertexDeclaration(declaration, &_effect_triangle_layout);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create effect vertex declaration! HRESULT is '" << std::hex << hr << std::dec << "'.";

			return false;
		}

		_gui.reset(new gui(this, nvgCreateD3D9(_device.get(), 0)));

		return runtime::on_init();
	}
	void d3d9_runtime::on_reset()
	{
		if (!_is_initialized)
		{
			return;
		}

		runtime::on_reset();

		// Destroy NanoVG
		nvgDeleteD3D9(_gui->context());
		_gui.reset();

		// Destroy resources
		_stateblock.reset();

		_backbuffer.reset();
		_backbuffer_resolved.reset();
		_backbuffer_texture.reset();
		_backbuffer_texture_surface.reset();

		_depthstencil.reset();
		_depthstencil_replacement.reset();
		_depthstencil_texture.reset();

		_default_depthstencil.reset();

		_effect_triangle_buffer.reset();
		_effect_triangle_layout.reset();

		// Clear depth source table
		for (auto &it : _depth_source_table)
		{
			LOG(TRACE) << "Removing depth-stencil " << it.first << " from list of possible depth candidates ...";

			it.first->Release();
		}

		_depth_source_table.clear();
	}
	void d3d9_runtime::on_present()
	{
		if (!_is_initialized)
		{
			LOG(TRACE) << "Failed to present! Runtime is in a lost state.";
			return;
		}

		detect_depth_source();

		// Begin post processing
		if (FAILED(_device->BeginScene()))
		{
			return;
		}

		// Capture device state
		_stateblock->Capture();

		BOOL software_rendering_enabled;
		D3DVIEWPORT9 viewport;
		com_ptr<IDirect3DSurface9> stateblock_rendertargets[8], stateblock_depthstencil;

		_device->GetViewport(&viewport);

		for (DWORD target = 0; target < _num_simultaneous_rendertargets; target++)
		{
			_device->GetRenderTarget(target, &stateblock_rendertargets[target]);
		}

		_device->GetDepthStencilSurface(&stateblock_depthstencil);

		if ((_behavior_flags & D3DCREATE_MIXED_VERTEXPROCESSING) != 0)
		{
			software_rendering_enabled = _device->GetSoftwareVertexProcessing();

			_device->SetSoftwareVertexProcessing(FALSE);
		}

		// Resolve back buffer
		if (_backbuffer_resolved != _backbuffer)
		{
			_device->StretchRect(_backbuffer.get(), nullptr, _backbuffer_resolved.get(), nullptr, D3DTEXF_NONE);
		}

		// Apply post processing
		on_apply_effect();

		// Reset render target
		_device->SetRenderTarget(0, _backbuffer_resolved.get());
		_device->SetDepthStencilSurface(_default_depthstencil.get());
		_device->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);

		// Apply presenting
		runtime::on_present();

		// Copy to back buffer
		if (_backbuffer_resolved != _backbuffer)
		{
			_device->StretchRect(_backbuffer_resolved.get(), nullptr, _backbuffer.get(), nullptr, D3DTEXF_NONE);
		}

		// Apply previous device state
		_stateblock->Apply();

		for (DWORD target = 0; target < _num_simultaneous_rendertargets; target++)
		{
			_device->SetRenderTarget(target, stateblock_rendertargets[target].get());
		}

		_device->SetDepthStencilSurface(stateblock_depthstencil.get());

		_device->SetViewport(&viewport);

		if ((_behavior_flags & D3DCREATE_MIXED_VERTEXPROCESSING) != 0)
		{
			_device->SetSoftwareVertexProcessing(software_rendering_enabled);
		}

		// End post processing
		_device->EndScene();
	}
	void d3d9_runtime::on_draw_call(D3DPRIMITIVETYPE type, UINT vertices)
	{
		switch (type)
		{
			case D3DPT_LINELIST:
				vertices *= 2;
				break;
			case D3DPT_LINESTRIP:
				vertices += 1;
				break;
			case D3DPT_TRIANGLELIST:
				vertices *= 3;
				break;
			case D3DPT_TRIANGLESTRIP:
			case D3DPT_TRIANGLEFAN:
				vertices += 2;
				break;
		}

		runtime::on_draw_call(vertices);

		com_ptr<IDirect3DSurface9> depthstencil;
		_device->GetDepthStencilSurface(&depthstencil);

		if (depthstencil != nullptr)
		{
			if (depthstencil == _depthstencil_replacement)
			{
				depthstencil = _depthstencil;
			}

			const auto it = _depth_source_table.find(depthstencil.get());

			if (it != _depth_source_table.end())
			{
				it->second.drawcall_count = static_cast<float>(_drawcalls);
				it->second.vertices_count += vertices;
			}
		}
	}
	void d3d9_runtime::on_apply_effect()
	{
		if (!_is_effect_compiled)
		{
			return;
		}

		_device->SetRenderTarget(0, _backbuffer_resolved.get());
		_device->SetDepthStencilSurface(nullptr);

		// Setup vertex input
		_device->SetStreamSource(0, _effect_triangle_buffer.get(), 0, sizeof(float));
		_device->SetVertexDeclaration(_effect_triangle_layout.get());

		// Apply post processing
		runtime::on_apply_effect();
	}
	void d3d9_runtime::on_apply_effect_technique(const technique &technique)
	{
		runtime::on_apply_effect_technique(technique);

		bool is_default_depthstencil_cleared = false;

		// Setup shader constants
		const auto uniform_storage_data = reinterpret_cast<const float *>(get_uniform_value_storage().data());
		_device->SetVertexShaderConstantF(0, uniform_storage_data, _constant_register_count);
		_device->SetPixelShaderConstantF(0, uniform_storage_data, _constant_register_count);

		for (const auto &pass_ptr : technique.passes)
		{
			const auto &pass = *static_cast<const d3d9_pass *>(pass_ptr.get());

			// Setup states
			pass.stateblock->Apply();

			// Save back buffer of previous pass
			_device->StretchRect(_backbuffer_resolved.get(), nullptr, _backbuffer_texture_surface.get(), nullptr, D3DTEXF_NONE);

			// Setup shader resources
			for (DWORD sampler = 0; sampler < pass.sampler_count; sampler++)
			{
				_device->SetTexture(sampler, pass.samplers[sampler].Texture->texture.get());

				for (DWORD state = D3DSAMP_ADDRESSU; state <= D3DSAMP_SRGBTEXTURE; state++)
				{
					_device->SetSamplerState(sampler, static_cast<D3DSAMPLERSTATETYPE>(state), pass.samplers[sampler].States[state]);
				}
			}

			// Setup render targets
			for (DWORD target = 0; target < _num_simultaneous_rendertargets; target++)
			{
				_device->SetRenderTarget(target, pass.render_targets[target]);
			}

			D3DVIEWPORT9 viewport;
			_device->GetViewport(&viewport);

			const float texelsize[4] = { -1.0f / viewport.Width, 1.0f / viewport.Height };
			_device->SetVertexShaderConstantF(255, texelsize, 1);

			const bool is_viewport_sized = viewport.Width == _width && viewport.Height == _height;

			_device->SetDepthStencilSurface(is_viewport_sized ? _default_depthstencil.get() : nullptr);

			if (is_viewport_sized && !is_default_depthstencil_cleared)
			{
				is_default_depthstencil_cleared = true;

				_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
			}
			else
			{
				_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0.0f, 0);
			}

			// Draw triangle
			_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);

			runtime::on_draw_call(3);

			// Update shader resources
			for (const auto target : pass.render_targets)
			{
				if (target == nullptr || target == _backbuffer_resolved)
				{
					continue;
				}

				com_ptr<IDirect3DBaseTexture9> texture;

				if (SUCCEEDED(target->GetContainer(IID_PPV_ARGS(&texture))) && texture->GetLevelCount() > 1)
				{
					texture->SetAutoGenFilterType(D3DTEXF_LINEAR);
					texture->GenerateMipSubLevels();
				}
			}
		}
	}

	void d3d9_runtime::on_set_depthstencil_surface(IDirect3DSurface9 *&depthstencil)
	{
		if (_depth_source_table.find(depthstencil) == _depth_source_table.end())
		{
			D3DSURFACE_DESC desc;
			depthstencil->GetDesc(&desc);

			// Early rejection
			if ((desc.Width < _width * 0.95 || desc.Width > _width * 1.05) || (desc.Height < _height * 0.95 || desc.Height > _height * 1.05) || desc.MultiSampleType != D3DMULTISAMPLE_NONE)
			{
				return;
			}
	
			LOG(TRACE) << "Adding depth-stencil " << depthstencil << " (Width: " << desc.Width << ", Height: " << desc.Height << ", Format: " << desc.Format << ") to list of possible depth candidates ...";

			depthstencil->AddRef();

			// Begin tracking
			const depth_source_info info = { desc.Width, desc.Height };
			_depth_source_table.emplace(depthstencil, info);
		}

		if (_depthstencil_replacement != nullptr && depthstencil == _depthstencil)
		{
			depthstencil = _depthstencil_replacement.get();
		}
	}
	void d3d9_runtime::on_get_depthstencil_surface(IDirect3DSurface9 *&depthstencil)
	{
		if (_depthstencil_replacement != nullptr && depthstencil == _depthstencil_replacement)
		{
			depthstencil->Release();

			depthstencil = _depthstencil.get();

			depthstencil->AddRef();
		}
	}

	void d3d9_runtime::screenshot(unsigned char *buffer) const
	{
		if (_backbuffer_format != D3DFMT_X8R8G8B8 &&
			_backbuffer_format != D3DFMT_X8B8G8R8 &&
			_backbuffer_format != D3DFMT_A8R8G8B8 &&
			_backbuffer_format != D3DFMT_A8B8G8R8)
		{
			LOG(WARNING) << "Screenshots are not supported for back buffer format " << _backbuffer_format << ".";
			return;
		}

		HRESULT hr;
		com_ptr<IDirect3DSurface9> screenshot_surface;

		hr = _device->CreateOffscreenPlainSurface(_width, _height, _backbuffer_format, D3DPOOL_SYSTEMMEM, &screenshot_surface, nullptr);

		if (FAILED(hr))
		{
			return;
		}

		hr = _device->GetRenderTargetData(_backbuffer_resolved.get(), screenshot_surface.get());

		if (FAILED(hr))
		{
			return;
		}

		D3DLOCKED_RECT mapped_rect;
		hr = screenshot_surface->LockRect(&mapped_rect, nullptr, D3DLOCK_READONLY);

		if (FAILED(hr))
		{
			return;
		}

		auto mapped_data = static_cast<BYTE *>(mapped_rect.pBits);
		const UINT pitch = _width * 4;

		for (UINT y = 0; y < _height; y++)
		{
			CopyMemory(buffer, mapped_data, std::min(pitch, static_cast<UINT>(mapped_rect.Pitch)));

			for (UINT x = 0; x < pitch; x += 4)
			{
				buffer[x + 3] = 0xFF;

				if (_backbuffer_format == D3DFMT_A8R8G8B8 || _backbuffer_format == D3DFMT_X8R8G8B8)
				{
					std::swap(buffer[x + 0], buffer[x + 2]);
				}
			}

			buffer += pitch;
			mapped_data += mapped_rect.Pitch;
		}

		screenshot_surface->UnlockRect();
	}
	bool d3d9_runtime::update_effect(const fx::nodetree &ast, const std::vector<std::string> &pragmas, std::string &errors)
	{
		bool skip_optimization = false;

		for (const auto &pragma : pragmas)
		{
			fx::lexer lexer(pragma);

			const auto prefix_token = lexer.lex();

			if (prefix_token.literal_as_string != "reshade")
			{
				continue;
			}

			const auto command_token = lexer.lex();

			if (command_token.literal_as_string == "skipoptimization" || command_token.literal_as_string == "nooptimization")
			{
				skip_optimization = true;
			}
		}

		return d3d9_fx_compiler(this, ast, errors, skip_optimization).run();
	}
	bool d3d9_runtime::update_texture(texture *texture, const unsigned char *data, size_t size)
	{
		const auto texture_impl = dynamic_cast<d3d9_texture *>(texture);

		assert(texture_impl != nullptr);
		assert(data != nullptr && size != 0);

		if (texture_impl->basetype != texture::datatype::image)
		{
			return false;
		}

		HRESULT hr;
		D3DSURFACE_DESC desc;
		texture_impl->texture->GetLevelDesc(0, &desc);

		com_ptr<IDirect3DTexture9> mem_texture;

		hr = _device->CreateTexture(desc.Width, desc.Height, 1, 0, desc.Format, D3DPOOL_SYSTEMMEM, &mem_texture, nullptr);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create memory texture for texture updating! HRESULT is '" << hr << "'.";

			return false;
		}

		D3DLOCKED_RECT mapped_rect;
		hr = mem_texture->LockRect(0, &mapped_rect, nullptr, 0);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to lock memory texture for texture updating! HRESULT is '" << hr << "'.";

			return false;
		}

		size = std::min(size, size_t(mapped_rect.Pitch * texture->height));
		auto mapped_data = static_cast<BYTE *>(mapped_rect.pBits);

		switch (texture->format)
		{
			case texture::pixelformat::r8:
				for (size_t i = 0; i < size; i += 1, mapped_data += 4)
				{
					mapped_data[0] = 0, mapped_data[1] = 0, mapped_data[2] = data[i], mapped_data[3] = 0;
				}
				break;
			case texture::pixelformat::rg8:
				for (size_t i = 0; i < size; i += 2, mapped_data += 4)
				{
					mapped_data[0] = 0, mapped_data[1] = data[i + 1], mapped_data[2] = data[i], mapped_data[3] = 0;
				}
				break;
			case texture::pixelformat::rgba8:
				for (size_t i = 0; i < size; i += 4, mapped_data += 4)
				{
					mapped_data[0] = data[i + 2], mapped_data[1] = data[i + 1], mapped_data[2] = data[i], mapped_data[3] = data[i + 3];
				}
				break;
			default:
				CopyMemory(mapped_data, data, size);
				break;
		}

		mem_texture->UnlockRect(0);

		hr = _device->UpdateTexture(mem_texture.get(), texture_impl->texture.get());

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to update texture from memory texture! HRESULT is '" << hr << "'.";

			return false;
		}

		return true;
	}
	void d3d9_runtime::update_texture_datatype(texture *texture, texture::datatype source, const com_ptr<IDirect3DTexture9> &newtexture)
	{
		const auto texture_impl = static_cast<d3d9_texture *>(texture);

		texture_impl->basetype = source;

		if (texture_impl->texture == newtexture)
		{
			return;
		}

		texture_impl->texture.reset();
		texture_impl->surface.reset();

		if (newtexture != nullptr)
		{
			texture_impl->texture = newtexture;
			newtexture->GetSurfaceLevel(0, &texture_impl->surface);

			D3DSURFACE_DESC desc;
			texture_impl->surface->GetDesc(&desc);

			texture_impl->width = desc.Width;
			texture_impl->height = desc.Height;
			texture_impl->format = texture::pixelformat::unknown;
			texture_impl->levels = newtexture->GetLevelCount();
		}
		else
		{
			texture_impl->width = texture_impl->height = texture_impl->levels = 0;
			texture_impl->format = texture::pixelformat::unknown;
		}
	}

	void d3d9_runtime::detect_depth_source()
	{
		static int cooldown = 0, traffic = 0;

		if (cooldown-- > 0)
		{
			traffic += s_network_traffic > 0;
			return;
		}
		else
		{
			cooldown = 30;

			if (traffic > 10)
			{
				traffic = 0;
				create_depthstencil_replacement(nullptr);
				return;
			}
			else
			{
				traffic = 0;
			}
		}

		if (_is_multisampling_enabled || _depth_source_table.empty())
		{
			return;
		}

		depth_source_info best_info = { 0 };
		IDirect3DSurface9 *best_match = nullptr;

		for (auto it = _depth_source_table.begin(); it != _depth_source_table.end();)
		{
			const auto depthstencil = it->first;
			auto &depthstencil_info = it->second;

			if ((depthstencil->AddRef(), depthstencil->Release()) == 1)
			{
				LOG(TRACE) << "Removing depth-stencil " << depthstencil << " from list of possible depth candidates ...";

				depthstencil->Release();

				it = _depth_source_table.erase(it);
				continue;
			}
			else
			{
				++it;
			}

			if (depthstencil_info.drawcall_count == 0)
			{
				continue;
			}

			if ((depthstencil_info.vertices_count * (1.2f - depthstencil_info.drawcall_count / _drawcalls)) >= (best_info.vertices_count * (1.2f - best_info.drawcall_count / _drawcalls)))
			{
				best_match = depthstencil;
				best_info = depthstencil_info;
			}

			depthstencil_info.drawcall_count = depthstencil_info.vertices_count = 0;
		}

		if (best_match != nullptr && _depthstencil != best_match)
		{
			LOG(TRACE) << "Switched depth source to depth-stencil " << best_match << ".";

			create_depthstencil_replacement(best_match);
		}
	}
	bool d3d9_runtime::create_depthstencil_replacement(IDirect3DSurface9 *depthstencil)
	{
		_depthstencil.reset();
		_depthstencil_replacement.reset();
		_depthstencil_texture.reset();

		if (depthstencil != nullptr)
		{
			_depthstencil = depthstencil;

			D3DSURFACE_DESC desc;
			_depthstencil->GetDesc(&desc);

			if (desc.Format != D3DFMT_INTZ)
			{
				const HRESULT hr = _device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_INTZ, D3DPOOL_DEFAULT, &_depthstencil_texture, nullptr);

				if (SUCCEEDED(hr))
				{
					_depthstencil_texture->GetSurfaceLevel(0, &_depthstencil_replacement);

					// Update auto depth-stencil
					com_ptr<IDirect3DSurface9> current_depthstencil;
					_device->GetDepthStencilSurface(&current_depthstencil);

					if (current_depthstencil != nullptr)
					{
						if (current_depthstencil == _depthstencil)
						{
							_device->SetDepthStencilSurface(_depthstencil_replacement.get());
						}
					}
				}
				else
				{
					LOG(TRACE) << "Failed to create depth-stencil replacement texture! HRESULT is '" << std::hex << hr << std::dec << "'. Are you missing support for the 'INTZ' format?";

					return false;
				}
			}
			else
			{
				_depthstencil_replacement = _depthstencil;

				_depthstencil_replacement->GetContainer(IID_PPV_ARGS(&_depthstencil_texture));
			}
		}

		// Update effect textures
		for (const auto &texture : _textures)
		{
			if (texture->basetype == texture::datatype::depthbuffer)
			{
				update_texture_datatype(texture.get(), texture::datatype::depthbuffer, _depthstencil_texture);
			}
		}

		return true;
	}
}