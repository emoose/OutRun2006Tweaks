#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <fstream>
#include <xxhash.h>
#include <d3d9.h>
#include <ddraw.h>
#include <unordered_set>

#define MAX_TEXTURE_CACHE_SIZE_MB (1024 + 256)

#define DDS_MAGIC 0x20534444  // "DDS "
struct DDS_FILE
{
	DWORD magic;
	DDSURFACEDESC2 data;
};

#define DDS_RGBA 0x41
#define DDS_RGB 0x40

inline D3DFORMAT GetD3DFormatFromPixelFormat(const DDPIXELFORMAT& format)
{
	if (format.dwFourCC == FOURCC_DXT1) return D3DFMT_DXT1;
	if (format.dwFourCC == FOURCC_DXT3) return D3DFMT_DXT3;
	if (format.dwFourCC == FOURCC_DXT5) return D3DFMT_DXT5;

	if (format.dwRGBBitCount == 16 &&
		format.dwRBitMask == 0x00000f00 &&
		format.dwGBitMask == 0x000000f0 &&
		format.dwBBitMask == 0x0000000f &&
		format.dwRGBAlphaBitMask == 0x0000f000)
	{
		return D3DFMT_A4R4G4B4;
	}
	if (format.dwRGBBitCount == 32 &&
		format.dwRBitMask == 0x00ff0000 &&
		format.dwGBitMask == 0x0000ff00 &&
		format.dwBBitMask == 0x000000ff &&
		format.dwRGBAlphaBitMask == 0xff000000)
	{
		return D3DFMT_A8R8G8B8;
	}
	if (format.dwRGBBitCount == 32 &&
		format.dwRBitMask == 0x000000ff &&
		format.dwGBitMask == 0x0000ff00 &&
		format.dwBBitMask == 0x00ff0000 &&
		format.dwRGBAlphaBitMask == 0xff000000)
	{
		return D3DFMT_A8B8G8R8; // not supported by most cards, will need conversion >.>
	}

	if (format.dwRGBBitCount == 16 &&
		format.dwRBitMask == 0x0000f800 &&
		format.dwGBitMask == 0x000007e0 &&
		format.dwBBitMask == 0x0000001f)
	{
		return D3DFMT_R5G6B5;
	}
	if (format.dwRGBBitCount == 24 &&
		format.dwRBitMask == 0x00ff0000 &&
		format.dwGBitMask == 0x0000ff00 &&
		format.dwBBitMask == 0x000000ff)
	{
		return D3DFMT_R8G8B8;
	}

	return D3DFMT_UNKNOWN;
}

size_t D3DXGetFormatSize(D3DFORMAT fmt, size_t width = 1, size_t height = 1)
{
	switch (fmt) {
	case D3DFMT_R8G8B8: return 3 * width * height;
	case D3DFMT_A8R8G8B8:
	case D3DFMT_A8B8G8R8:
	case D3DFMT_X8R8G8B8:
	case D3DFMT_X8B8G8R8: return 4 * width * height;
	case D3DFMT_R5G6B5:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
	case D3DFMT_A4R4G4B4: return 2 * width * height;
	case D3DFMT_A8:
	case D3DFMT_P8:
	case D3DFMT_L8: return width * height;
	case D3DFMT_A8P8:
	case D3DFMT_A8L8:
	case D3DFMT_V8U8:
	case D3DFMT_L6V5U5: return 2 * width * height;
	case D3DFMT_X8L8V8U8:
	case D3DFMT_Q8W8V8U8:
	case D3DFMT_V16U16:
	case D3DFMT_D32:
	case D3DFMT_D24S8:
	case D3DFMT_D24X8:
	case D3DFMT_D24X4S4:
	case D3DFMT_D32F_LOCKABLE:
	case D3DFMT_R32F:
	case D3DFMT_G32R32F: return 4 * width * height;
	case D3DFMT_A32B32G32R32F: return 16 * width * height;
	case D3DFMT_A16B16G16R16F: return 8 * width * height;
	case D3DFMT_R16F:
	case D3DFMT_D16_LOCKABLE:
	case D3DFMT_D16: return 2 * width * height;
	case D3DFMT_D15S1: return 2 * width * height;
	case D3DFMT_DXT1: return ((width + 3) / 4) * ((height + 3) / 4) * 8;
	case D3DFMT_DXT3:
	case D3DFMT_DXT5: return ((width + 3) / 4) * ((height + 3) / 4) * 16;
	default:
		assert(false && "Unsupported format");
		return 0;
	}
}

#define D3DX_FILTER_NONE                 0x00000001
#define D3DX_FILTER_POINT                0x00000002
#define D3DX_FILTER_LINEAR               0x00000003
#define D3DX_FILTER_TRIANGLE             0x00000004
#define D3DX_FILTER_BOX                  0x00000005
#define D3DX_FILTER_MIRROR_U             0x00010000
#define D3DX_FILTER_MIRROR_V             0x00020000
#define D3DX_FILTER_MIRROR_W             0x00040000
#define D3DX_FILTER_MIRROR               0x00070000
#define D3DX_FILTER_DITHER               0x00080000
#define D3DX_FILTER_DITHER_DIFFUSION     0x00100000
#define D3DX_FILTER_SRGB_IN              0x00200000
#define D3DX_FILTER_SRGB_OUT             0x00400000
#define D3DX_FILTER_SRGB                 0x00600000

// Simplified version of D3DXCreateTextureFromFileInMemoryEx which allows loading textures much faster
HRESULT D3DXCreateTextureFromFileInMemoryEx_Custom(
	IDirect3DDevice9* pDevice,
	const void* pData,
	size_t dataSize,
	UINT Width,
	UINT Height,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	DWORD Filter,
	DWORD MipFilter,
	LPDIRECT3DTEXTURE9* ppTexture)
{
	if (!pDevice || !pData || !ppTexture)
		return E_POINTER;

	const uint8_t* data = static_cast<const uint8_t*>(pData);
	const DDS_FILE* header = reinterpret_cast<const DDS_FILE*>(data);

	// Validate DDS header
	if (header->magic != DDS_MAGIC)
		return E_FAIL;

	// Extract texture information
	Width = (Width != D3DX_DEFAULT) ? Width : header->data.dwWidth;
	if (Width > header->data.dwWidth)
		Width = header->data.dwWidth;

	Height = (Height != D3DX_DEFAULT) ? Height : header->data.dwHeight;
	if (Height > header->data.dwHeight)
		Height = header->data.dwHeight;

	MipLevels = (MipLevels != D3DX_DEFAULT) ? MipLevels : header->data.dwMipMapCount;
	if (MipLevels > header->data.dwMipMapCount)
		MipLevels = header->data.dwMipMapCount;

	D3DFORMAT format_orig = GetD3DFormatFromPixelFormat(header->data.ddpfPixelFormat);
	if (format_orig == D3DFMT_UNKNOWN)
		return E_FAIL;

	D3DFORMAT format_present = format_orig;
	if (format_orig == D3DFMT_A8B8G8R8)
		format_present = D3DFMT_A8R8G8B8;

	// Create the texture
	HRESULT hr = pDevice->CreateTexture(
		Width,
		Height,
		MipLevels,
		Usage,
		format_present,
		Pool,
		ppTexture,
		nullptr
	);

	if (FAILED(hr))
		return hr;

	// Lock the texture and copy data
	D3DLOCKED_RECT lockedRect;
	uint8_t* srcData = const_cast<uint8_t*>(data) + sizeof(DDS_FILE);
	for (UINT mipLevel = 0; mipLevel < MipLevels; ++mipLevel)
	{
		hr = (*ppTexture)->LockRect(mipLevel, &lockedRect, nullptr, D3DLOCK_DISCARD);
		if (FAILED(hr))
		{
			(*ppTexture)->Release();
			return hr;
		}

		// Calculate mip size
		UINT mipWidth = max(1U, Width >> mipLevel);
		UINT mipHeight = max(1U, Height >> mipLevel);
		size_t mipSize = D3DXGetFormatSize(format_present, mipWidth, mipHeight);

		if (format_orig == D3DFMT_A8B8G8R8)
		{
			// Convert A8B8G8R8 to A8R8G8B8
			uint8_t* destData = static_cast<uint8_t*>(lockedRect.pBits);
			for (UINT y = 0; y < mipHeight; ++y) {
				for (UINT x = 0; x < mipWidth; ++x) {
					uint8_t b = srcData[4 * (y * mipWidth + x)];
					uint8_t g = srcData[4 * (y * mipWidth + x) + 1];
					uint8_t r = srcData[4 * (y * mipWidth + x) + 2];
					uint8_t a = srcData[4 * (y * mipWidth + x) + 3];
					destData[4 * (y * mipWidth + x)] = r;
					destData[4 * (y * mipWidth + x) + 1] = g;
					destData[4 * (y * mipWidth + x) + 2] = b;
					destData[4 * (y * mipWidth + x) + 3] = a;
				}
			}
		}
		else {
			// Copy image data to the texture directly
			memcpy(lockedRect.pBits, srcData, mipSize);
		}

		(*ppTexture)->UnlockRect(mipLevel);

		// Move to the next mip level
		srcData += mipSize;
	}

	// Apply filtering modes (just the ones used by C2C)
	// TODO: this likely isn't applying filtering properly, we probably need to gen mipmaps & use D3DXFilterTexture...

	if (Filter != 0) {
		if (Filter == D3DX_FILTER_NONE)
			Filter = D3DTEXF_NONE;
		else if (Filter == D3DX_FILTER_LINEAR)
			Filter = D3DTEXF_LINEAR;
		pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, Filter);
		pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, Filter);
	}
	if (MipFilter != 0) {
		if (MipFilter == D3DX_FILTER_NONE)
			MipFilter = D3DTEXF_NONE;
		else if (MipFilter == D3DX_FILTER_LINEAR)
			MipFilter = D3DTEXF_LINEAR;
		pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, MipFilter);
	}

	return S_OK;
}

class FileDataCache
{
private:
	struct CacheEntry
	{
		std::vector<uint8_t> data;
		std::list<std::filesystem::path>::iterator lru_iterator;
	};

	std::size_t max_cache_size;
	std::size_t current_cache_size;
	std::unordered_map<std::filesystem::path, CacheEntry> cache;
	std::list<std::filesystem::path> lru_list;

	void evict()
	{
		while (!lru_list.empty() && current_cache_size > max_cache_size)
		{
			std::filesystem::path lru_file = lru_list.back();
			current_cache_size -= cache[lru_file].data.size();
			cache.erase(lru_file);
			lru_list.pop_back();
		}
	}

	std::mutex mtx1;
	std::mutex mtx2;
	std::mutex mtx3;

public:
	FileDataCache(std::size_t maxCacheSize) : max_cache_size(maxCacheSize), current_cache_size(0) {}

	void cacheFolder(std::filesystem::path folder)
	{
		if (std::filesystem::exists(folder))
			for (const auto& entry : std::filesystem::directory_iterator(folder))
				if (entry.is_regular_file())
					cacheFile(entry.path());
	}

	void cacheFile(std::filesystem::path filename)
	{
		std::lock_guard _(mtx1);

		if (cache.find(filename) != cache.end())
		{
			// File is already cached, move it to the front of LRU list
			lru_list.erase(cache[filename].lru_iterator);
			lru_list.push_front(filename);
			cache[filename].lru_iterator = lru_list.begin();
			return;
		}

		std::ifstream file(filename, std::ios::binary | std::ios::ate);
		if (!file)
		{
			throw std::runtime_error("Unable to open file: " + filename.string());
		}

		std::streamsize size = file.tellg();
		file.seekg(0, std::ios::beg);

		std::vector<uint8_t> buffer(size);
		if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
		{
			throw std::runtime_error("Error reading file: " + filename.string());
		}		
		else
		{
			current_cache_size += size;
			evict();

			if (size > max_cache_size)
				throw std::runtime_error("File size exceeds maximum cache size");

			lru_list.push_front(filename);
			cache[filename] = { std::move(buffer), lru_list.begin() };
		}
	}

	const uint8_t* getFileData(std::filesystem::path filename, size_t* size)
	{
		std::lock_guard _(mtx2);
		auto it = cache.find(filename);
		if (it == cache.end())
		{
#ifdef _DEBUG
			std::string msg = "Cache miss: " + filename.string() + "\n";
			OutputDebugStringA(msg.c_str());
#endif
			cacheFile(filename);
			it = cache.find(filename);
			if (it == cache.end())
				return nullptr;
		}

		// Move the accessed file to the front of the LRU list
		lru_list.erase(it->second.lru_iterator);
		lru_list.push_front(filename);
		it->second.lru_iterator = lru_list.begin();

		if (size)
			*size = it->second.data.size();

		return it->second.data.data();
	}

	std::size_t getCacheSize() const
	{
		return current_cache_size;
	}
};

// QnD file entry cache class, so we don't have to run std::filesystem::exists for every texture
// Probably not that much benefit since OS should have the directory cached anyway, but reducing OS calls should always help
class DirectoryFileCache
{
public:
	DirectoryFileCache() {}
	explicit DirectoryFileCache(const std::filesystem::path& directory)
		: directoryPath(directory)
	{
		scanDirectory();
	}

	bool exists(const std::filesystem::path& filename) const
	{
		return fileSet.find(filename) != fileSet.end();
	}

	void refresh()
	{
		fileSet.clear();
		scanDirectory();
	}

private:
	std::filesystem::path directoryPath;
	std::unordered_set<std::filesystem::path> fileSet;

	void scanDirectory()
	{
		if (!std::filesystem::exists(directoryPath))
			return;

		for (const auto& entry : std::filesystem::recursive_directory_iterator(directoryPath))
			if (entry.is_regular_file() || entry.is_directory())
				fileSet.insert(entry.path());
	}
};

class TextureReplacement : public Hook
{
	inline static std::filesystem::path XmtDumpPath;
	inline static std::filesystem::path XmtLoadPath;
	inline static DirectoryFileCache FileSystem;
	inline static FileDataCache FileData = FileDataCache(MAX_TEXTURE_CACHE_SIZE_MB * 1024 * 1024);

	// Remappings for FXT modded sprites, so we can point them toward the vanilla versions
	inline static std::unordered_map<uint32_t, std::tuple<uint32_t, int, int>> FxtHashRemappings =
	{
		// spr_sprani_etc_cvt_Exst
		{0x8F1D69D3, {0x1F77CB88,128,128}}, // FXT arcade controls

		// spr_sprani_CLAR_RANK_Exst
		{0x8F663222, {0x4AFC1BED,512,512}}, // uncensored Clarissa
		{0x469F516D, {0xF043316B,1024,512}}, // uncensored Clarissa
		{0x86325B15, {0x8B52FEEC,1024,512}}, // uncensored Clarissa

		// spr_sprani_game_cvt_Exst
		{0x1025284E, {0xC4A2937B,1024,1024}}, // uncensored Clarissa
		{0xFDC4E73B, {0x39229D64,1024,1024}}, // uncensored Clarissa

		// spr_sprani_sumo_fe_cvt_Exst
		{0x50D487A9, {0xACF61D7C,1024,512}}, // arcade controls
		{0x2F7DFED1, {0x31C58963,512,256}}, // removed "press enter" text
		{0x678170B1, {0x4DC09A74,1024,1024}}, // uncensored Clarissa

		// spr_sprani_selector_cvt_Exst
		{0x698EEA48, {0x2DA43E41,1024,1024}}, // uncensored Clarissa

		// TODO: there is an FXT spr_sprani_congrats_cvt_exst but not sure how to get game to load that yet..
	};

	//
	// UI texture replacement code
	//

	inline static std::filesystem::path CurrentXstsetFilename;
	inline static int CurrentXstsetIndex = 0;

	inline static std::unordered_map<int, std::pair<float, float>> sprite_scales;

	// put_sprite_ex2 usually doesn't have the proper textureId set inside SPRARGS2, only the d3dtexture_ptr_C
	// we could store each d3dtexture ptr somewhere when they're created, and then check against them later to find the scale
	// but that's not really ideal since pointers could get freed, with new texture using same address
	// luckily it seems almost every put_sprite_ex2 call is preceeded by a get_texture call with the actual texture ID
	// 
	// so we can just hook get_texture and check the textureId arg against our textureId -> scale map
	// if it exists we'll stash the d3d pointer -> textureId mapping temporarily for put_sprite_ex2 to use
	inline static void* prevTexture = nullptr;
	inline static int prevTextureId = 0;

	// Mask textures don't get put_sprite_ex2 called for them, instead the next sprite drawn will have ptr_B4 setup to point to it
	// That next sprite will likely call get_texture_dest with a different sprite ID, so we also have to keep track of the ID used prior
	inline static void* prevMaskTexture = nullptr;
	inline static int prevMaskTextureId = 0;

	inline static SafetyHookInline get_texture = {};
	static void* __cdecl get_texture_dest(int textureId)
	{
		auto* ret = get_texture.call<void*>(textureId);

		prevMaskTexture = prevTexture;
		prevMaskTextureId = prevTextureId;

		if (ret && sprite_scales.contains(textureId))
		{
			prevTexture = ret;
			prevTextureId = textureId;
		}
		else
		{
			prevTexture = nullptr;
			prevTextureId = 0;
		}
		return ret;
	}

	static void RescaleSprArgs2(SPRARGS2* a1, float scaleX, float scaleY)
	{
		// left is kept at both unk_88 & unk_A0, we'll do both seperately in case they're different for some reason
		float origLeft = 1.0f - a1->left_88;
		origLeft = origLeft * scaleX;
		a1->left_88 = 1.0f - origLeft;

		origLeft = 1.0f - a1->left_A0;
		origLeft = origLeft * scaleX;
		a1->left_A0 = 1.0f - origLeft;

		// right at unk_90 & unk_98
		float origRight = 1.0f - a1->right_90;
		origRight = origRight * scaleX;
		a1->right_90 = 1.0f - origRight;

		origRight = 1.0f - a1->right_98;
		origRight = origRight * scaleX;
		a1->right_98 = 1.0f - origRight;

		// bottom?
		a1->bottom_84 = a1->bottom_84 * scaleY;
		a1->bottom_8C = a1->bottom_8C * scaleY;

		// top?
		a1->top_94 = a1->top_94 * scaleY;
		a1->top_9C = a1->top_9C * scaleY;
	}

	inline static SafetyHookInline put_sprite_ex2 = {};
	static int __cdecl put_sprite_ex2_dest(SPRARGS2* a1, float a2)
	{
		int xstnum = a1->xstnum_0;

		if (a1->d3dtexture_ptr_C == prevTexture && sprite_scales.contains(prevTextureId))
		{
			auto scaleX = std::get<0>(sprite_scales[prevTextureId]);
			auto scaleY = std::get<1>(sprite_scales[prevTextureId]);
			RescaleSprArgs2(a1, scaleX, scaleY);
			prevTexture = nullptr;
			prevTextureId = 0;
		}

		if (a1->child_B4)
		{
			int spr_mask_flag = *Module::exe_ptr<int>(0x586B28);
			if (spr_mask_flag)
			{
				if (a1->child_B4->d3dtexture_ptr_C == prevMaskTexture && sprite_scales.contains(prevMaskTextureId))
				{
					auto scaleX = std::get<0>(sprite_scales[prevMaskTextureId]);
					auto scaleY = std::get<1>(sprite_scales[prevMaskTextureId]);
					RescaleSprArgs2(a1->child_B4, scaleX, scaleY);
					prevMaskTexture = nullptr;
					prevMaskTextureId = 0;
				}
			}
		}

		return put_sprite_ex2.call<int>(a1, a2);
	}

	inline static SafetyHookInline put_sprite_ex = {};
	static int __cdecl put_sprite_ex_dest(SPRARGS* a1, float a2)
	{
		int xstnum = a1->xstnum_0;
		if (sprite_scales.contains(xstnum))
		{
			auto scaleX = std::get<0>(sprite_scales[xstnum]);
			auto scaleY = std::get<1>(sprite_scales[xstnum]);
			a1->top_4 = a1->top_4 * scaleY;
			a1->left_8 = a1->left_8 * scaleX;
			a1->bottom_C = a1->bottom_C * scaleY;
			a1->right_10 = a1->right_10 * scaleX;
			a1->scaleX = a1->scaleX / scaleX;
			a1->scaleY = a1->scaleY / scaleY;
		}
		return put_sprite_ex.call<int>(a1, a2);
	}

	inline static SafetyHookMid LoadXstsetSprite_hook = {};
	static void LoadXstsetSprite_dest(safetyhook::Context& ctx)
	{
		CurrentXstsetFilename = (char*)ctx.ecx; // sprite xstset filename
		CurrentXstsetIndex = (int)(ctx.eax); // index into xstset array
	};

	static void HandleTexture(void** ppSrcData, UINT* pSrcDataSize, std::filesystem::path texturePackName, bool isUITexture)
	{
		if (!*ppSrcData || !*pSrcDataSize)
			return;

		bool allowReplacement = isUITexture ? Settings::UITextureReplacement : Settings::SceneTextureReplacement;
		bool allowDump = isUITexture ? Settings::UITextureDump : Settings::SceneTextureDump;

		const DDS_FILE* header = (const DDS_FILE*)*ppSrcData;
		if (header->magic != DDS_MAGIC)
			return;

		int width = header->data.dwWidth;
		int height = header->data.dwHeight;
		auto hash = XXH32(*ppSrcData, *pSrcDataSize, 0);

		// Remap some modified FXT textures to their original hashes
		if (FxtHashRemappings.count(hash))
		{
			auto& mapping = FxtHashRemappings[hash];
			if (std::get<1>(mapping) == width && std::get<2>(mapping) == height) // make sure width/height match expected in case of collision...
				hash = std::get<0>(mapping);
		}

		std::string ddsName = std::format("{:X}_{}x{}.dds", hash, width, height);

		bool dumpTexture = true;
		if (allowReplacement)
		{
			auto path_load = XmtLoadPath / texturePackName.filename().stem() / ddsName;
			if (!FileSystem.exists(path_load))
				path_load = XmtLoadPath / ddsName;

			if (FileSystem.exists(path_load))
			{
				size_t size = 0;
				const uint8_t* file = FileData.getFileData(path_load, &size);
				if (file)
				{
					const DDS_FILE* newhead = (const DDS_FILE*)file;
					if (newhead->magic == DDS_MAGIC)
					{
						if (isUITexture)
						{
							// Calc the scaling ratio of new texture vs old one, to use in put_sprite funcs later
							float ratio_width = float(newhead->data.dwWidth) / float(header->data.dwWidth);
							float ratio_height = float(newhead->data.dwHeight) / float(header->data.dwHeight);

							int curTextureNum = *Module::exe_ptr<int>(0x55B25C);
							sprite_scales[(CurrentXstsetIndex << 16) | curTextureNum] = { ratio_width, ratio_height };
						}

						// Replace header in the old data in case some game code tries reading it...
						//memcpy(*ppSrcData, file, sizeof(DDS_FILE));

						// Update pointers to our new texture
						*ppSrcData = (void*)file;
						*pSrcDataSize = size;

						// Don't dump texture if we've loaded in new one
						dumpTexture = false;
					}
				}
			}
		}

		if (allowDump && dumpTexture)
		{
			auto path_dump = XmtDumpPath / texturePackName.filename().stem();
			if (!FileSystem.exists(path_dump))
				std::filesystem::create_directories(path_dump);

			path_dump = path_dump / ddsName;
			if (!FileSystem.exists(path_dump))
			{
				std::ofstream file(path_dump, std::ios::binary);
				if (file)
				{
					file.write((const char*)*ppSrcData, *pSrcDataSize);
					FileSystem.refresh();
				}
			}
		}
	}

	// Two versions of the func depending on Settings::UseNewTextureAllocator, to reduce branching
	inline static SafetyHookInline D3DXCreateTextureFromFileInMemory = {};
	static HRESULT __stdcall D3DXCreateTextureFromFileInMemory_Custom_dest(LPDIRECT3DDEVICE9 pDevice, void* pSrcData, UINT SrcDataSize, LPDIRECT3DTEXTURE9* ppTexture)
	{
		if (pSrcData && SrcDataSize)
		{
			HandleTexture(&pSrcData, &SrcDataSize, CurrentXstsetFilename, true);
		}

		// Call D3DXCreateTextureFromFileInMemoryEx instead of D3DXCreateTextureFromFileInMemory, so we can specify no mipmaps
		// Should prevent D3D from trying to generate mipmaps, reducing load times of non-mipped UI textures quite a bit
		return D3DXCreateTextureFromFileInMemoryEx_Custom(pDevice, pSrcData, SrcDataSize, D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, 1, 3, ppTexture);
	}
	static HRESULT __stdcall D3DXCreateTextureFromFileInMemory_Orig_dest(LPDIRECT3DDEVICE9 pDevice, void* pSrcData, UINT SrcDataSize, LPDIRECT3DTEXTURE9* ppTexture)
	{
		if (pSrcData && SrcDataSize)
		{
			HandleTexture(&pSrcData, &SrcDataSize, CurrentXstsetFilename, true);
		}

		// Call D3DXCreateTextureFromFileInMemoryEx instead of D3DXCreateTextureFromFileInMemory, so we can specify no mipmaps
		// Should prevent D3D from trying to generate mipmaps, reducing load times of non-mipped UI textures quite a bit
		return D3DXCreateTextureFromFileInMemoryEx.stdcall<HRESULT>(pDevice, pSrcData, SrcDataSize, D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, 1, 3, 0, nullptr, nullptr, ppTexture);
	}

	//
	// Scene texture replacement code
	//

	inline static std::filesystem::path CurrentXmtsetFilename;
	inline static const char* PrevXmtName = nullptr;

	// Two versions of the func depending on Settings::UseNewTextureAllocator, to reduce branching
	inline static SafetyHookInline D3DXCreateTextureFromFileInMemoryEx = {};
	static HRESULT __stdcall D3DXCreateTextureFromFileInMemoryEx_Custom_dest(LPDIRECT3DDEVICE9 pDevice, void* pSrcData, UINT SrcDataSize, UINT Width, UINT Height, UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, void* pSrcInfo, PALETTEENTRY* pPalette, LPDIRECT3DTEXTURE9* ppTexture)
	{
		if ((Settings::SceneTextureReplacement || Settings::SceneTextureDump) && pSrcData && SrcDataSize)
		{
			HandleTexture(&pSrcData, &SrcDataSize, CurrentXmtsetFilename, false);
		}

		return D3DXCreateTextureFromFileInMemoryEx_Custom(pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter, ppTexture);
	}
	static HRESULT __stdcall D3DXCreateTextureFromFileInMemoryEx_Orig_dest(LPDIRECT3DDEVICE9 pDevice, void* pSrcData, UINT SrcDataSize, UINT Width, UINT Height, UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, void* pSrcInfo, PALETTEENTRY* pPalette, LPDIRECT3DTEXTURE9* ppTexture)
	{
		if ((Settings::SceneTextureReplacement || Settings::SceneTextureDump) && pSrcData && SrcDataSize)
		{
			HandleTexture(&pSrcData, &SrcDataSize, CurrentXmtsetFilename, false);
		}

		return D3DXCreateTextureFromFileInMemoryEx.stdcall<HRESULT>(pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
	}

	inline static SafetyHookInline LoadXmtsetObject = {};
	static int __cdecl LoadXmtsetObject_dest(char* XmtFileName, int XmtIndex)
	{
		if (PrevXmtName != XmtFileName)
		{
			CurrentXmtsetFilename = XmtFileName;
			PrevXmtName = XmtFileName;
		}

		return LoadXmtsetObject.call<int>(XmtFileName, XmtIndex);
	}

	inline static std::atomic<bool> isComplete{ true };
	inline static std::thread cachingThread;
	static void cacheFolderFiles(const std::filesystem::path& xmtFileName)
	{
		auto fileName = xmtFileName.stem();
		auto folderPath = XmtLoadPath / fileName;

#ifdef _DEBUG
		std::string msg = "cacheFolderFiles: " + folderPath.string() + "\n";
		OutputDebugStringA(msg.c_str());
#endif

		FileData.cacheFolder(folderPath);

		isComplete = true;
	}

	inline static SafetyHookMid LoadXmtsetObject_Step1 = {};
	static void __cdecl LoadXmtsetObject_Step1_dest(SafetyHookContext& ctx)
	{
		if (!isComplete)
			return;

		isComplete = false;
		cachingThread = std::thread(cacheFolderFiles, (const char*)ctx.esi);
		cachingThread.detach();
	}

	inline static SafetyHookMid LoadXmtsetObject_Step3 = {};
	static void __cdecl LoadXmtsetObject_Step3_dest(SafetyHookContext& ctx)
	{
		if (!isComplete)
			ctx.eax = 0;
	}


public:
	std::string_view description() override
	{
		return "TextureReplacement";
	}

	bool validate() override
	{
		return (Settings::SceneTextureReplacement || Settings::SceneTextureDump) || (Settings::UITextureReplacement || Settings::UITextureDump);
	}

	bool apply() override
	{
		const static int D3DXCreateTextureFromFileInMemory_Addr = 0x39412;
		const static int get_texture_Addr = 0x2A030;
		const static int put_sprite_ex_Addr = 0x2CFE0;
		const static int put_sprite_ex2_Addr = 0x2D0C0;
		const static int LoadXstsetSprite_Addr = 0x2FE20;

		const static int D3DXCreateTextureFromFileInMemoryEx_Addr = 0x39406;
		const static int LoadXmtsetObject_Addr = 0x2E0D0;

		const static int LoadXmtsetObject_Step1_HookAddr = 0x2E169;
		const static int LoadXmtsetObject_Step3_HookAddr = 0x2E304;

		XmtDumpPath = "textures\\dump";
		XmtLoadPath = "textures\\load";

		// Startup texture cache, causes game to take a while to boot, disabled for now...
#if 0
		if (std::filesystem::exists(XmtLoadPath))
		{
			for (const auto& entry : std::filesystem::recursive_directory_iterator(XmtLoadPath))
			{
				if (entry.is_regular_file()) {
					FileData.cacheFile(entry.path());
				}
			}
			std::string msg = "Initial cache size: " + std::to_string(FileData.getCacheSize());
			OutputDebugStringA(msg.c_str());
		}
#endif

		FileSystem = DirectoryFileCache(XmtLoadPath);

		bool ApplyUIHooks = Settings::UITextureReplacement || Settings::UITextureDump;
		bool ApplySceneHooks = Settings::SceneTextureReplacement || Settings::SceneTextureDump;

		// Scene hooks are applied through D3DXCreateTextureFromFileInMemoryEx
		// But our UI code also calls D3DXCreateTextureFromFileInMemoryEx to allow loading textures slightly faster
		// So we'll setup this hook if either are enabled
		if (ApplyUIHooks || ApplySceneHooks)
		{
			if (Settings::UseNewTextureAllocator)
				D3DXCreateTextureFromFileInMemoryEx = safetyhook::create_inline(Module::exe_ptr(D3DXCreateTextureFromFileInMemoryEx_Addr), D3DXCreateTextureFromFileInMemoryEx_Custom_dest);
			else
				D3DXCreateTextureFromFileInMemoryEx = safetyhook::create_inline(Module::exe_ptr(D3DXCreateTextureFromFileInMemoryEx_Addr), D3DXCreateTextureFromFileInMemoryEx_Orig_dest);
		}

		if (ApplyUIHooks)
		{
			if(Settings::UseNewTextureAllocator)
				D3DXCreateTextureFromFileInMemory = safetyhook::create_inline(Module::exe_ptr(D3DXCreateTextureFromFileInMemory_Addr), D3DXCreateTextureFromFileInMemory_Custom_dest);
			else
				D3DXCreateTextureFromFileInMemory = safetyhook::create_inline(Module::exe_ptr(D3DXCreateTextureFromFileInMemory_Addr), D3DXCreateTextureFromFileInMemory_Orig_dest);

			LoadXstsetSprite_hook = safetyhook::create_mid(Module::exe_ptr(LoadXstsetSprite_Addr), LoadXstsetSprite_dest);

			if (Settings::UITextureReplacement)
			{
				get_texture = safetyhook::create_inline(Module::exe_ptr(get_texture_Addr), get_texture_dest);
				put_sprite_ex = safetyhook::create_inline(Module::exe_ptr(put_sprite_ex_Addr), put_sprite_ex_dest);
				put_sprite_ex2 = safetyhook::create_inline(Module::exe_ptr(put_sprite_ex2_Addr), put_sprite_ex2_dest);
			}
		}

		if (ApplySceneHooks)
		{
			LoadXmtsetObject = safetyhook::create_inline(Module::exe_ptr(LoadXmtsetObject_Addr), LoadXmtsetObject_dest);

			if (Settings::EnableTextureCache)
			{
				LoadXmtsetObject_Step1 = safetyhook::create_mid(Module::exe_ptr(LoadXmtsetObject_Step1_HookAddr), LoadXmtsetObject_Step1_dest);
				LoadXmtsetObject_Step3 = safetyhook::create_mid(Module::exe_ptr(LoadXmtsetObject_Step3_HookAddr), LoadXmtsetObject_Step3_dest);
			}
		}

		return true;
	}

	static TextureReplacement instance;
};
TextureReplacement TextureReplacement::instance;
