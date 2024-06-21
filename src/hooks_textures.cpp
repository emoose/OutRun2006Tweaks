#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <fstream>
#include <xxhash.h>
#include <d3d9.h>
#include <ddraw.h>
#include <unordered_set>

#define DDS_MAGIC 0x20534444  // "DDS "
struct DDS_FILE
{
	DWORD magic;
	DDSURFACEDESC2 data;
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

	//
	// UI texture replacement code
	//

	const static int D3DXCreateTextureFromFileInMemory_Addr = 0x39412;
	const static int get_texture_Addr = 0x2A030;
	const static int put_sprite_ex_Addr = 0x2CFE0;
	const static int put_sprite_ex2_Addr = 0x2D0C0;
	const static int LoadXstsetSprite_Addr = 0x2FE20;

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

	inline static SafetyHookInline get_texture = {};
	static void* __cdecl get_texture_dest(int textureId)
	{
		auto* ret = get_texture.call<void*>(textureId);
		if (ret && sprite_scales.contains(textureId))
		{
			prevTexture = ret;
			prevTextureId = textureId;
		}
		return ret;
	}

	inline static SafetyHookInline put_sprite_ex2 = {};
	static int __cdecl put_sprite_ex2_dest(SPRARGS2* a1, float a2)
	{
		int xstnum = a1->xstnum_0;
		if (a1->d3dtexture_ptr_C == prevTexture && sprite_scales.contains(prevTextureId))
		{
			auto scaleX = std::get<0>(sprite_scales[prevTextureId]);
			auto scaleY = std::get<1>(sprite_scales[prevTextureId]);

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

			prevTexture = nullptr;
			prevTextureId = 0;
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

	static std::unique_ptr<uint8_t[]> HandleTexture(void** ppSrcData, UINT* pSrcDataSize, std::filesystem::path texturePackName, bool isUITexture)
	{
		if (!*ppSrcData || !*pSrcDataSize)
			return nullptr;

		bool allowReplacement = isUITexture ? Settings::UITextureReplacement : Settings::SceneTextureReplacement;
		bool allowDump = isUITexture ? Settings::UITextureDump : Settings::SceneTextureDump;

		const DDS_FILE* header = (const DDS_FILE*)*ppSrcData;
		if (header->magic != DDS_MAGIC)
			return nullptr;

		std::unique_ptr<uint8_t[]> NewSrcData;

		int width = header->data.dwWidth;
		int height = header->data.dwHeight;
		auto hash = XXH32(*ppSrcData, *pSrcDataSize, 0);

		std::string ddsName = std::format("{:X}_{}x{}.dds", hash, width, height);

		bool dumpTexture = true;
		if (allowReplacement)
		{
			auto path_load = XmtLoadPath / texturePackName.filename().stem() / ddsName;
			if (!FileSystem.exists(path_load))
				path_load = XmtLoadPath / ddsName;

			if (FileSystem.exists(path_load))
			{
				std::ifstream file(path_load, std::ios::binary | std::ios::ate);
				if (file)
				{
					std::streamsize size = file.tellg();
					file.seekg(0, std::ios::beg);

					NewSrcData = std::make_unique<uint8_t[]>(size);
					if (file.read((char*)NewSrcData.get(), size))
					{
						const DDS_FILE* newhead = (const DDS_FILE*)NewSrcData.get();
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
							memcpy(*ppSrcData, NewSrcData.get(), sizeof(DDS_FILE));

							// Update pointers to our new texture
							*ppSrcData = NewSrcData.get();
							*pSrcDataSize = size;

							// Don't dump texture if we've loaded in new one
							dumpTexture = false;
						}
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

		return std::move(NewSrcData);
	}


	inline static SafetyHookInline D3DXCreateTextureFromFileInMemory = {};
	static HRESULT __stdcall D3DXCreateTextureFromFileInMemory_dest(LPDIRECT3DDEVICE9 pDevice, void* pSrcData, UINT SrcDataSize, LPDIRECT3DTEXTURE9* ppTexture)
	{
		std::unique_ptr<uint8_t[]> newdata;

		if (pSrcData && SrcDataSize)
		{
			newdata = HandleTexture(&pSrcData, &SrcDataSize, CurrentXstsetFilename, true);
		}

		// Call D3DXCreateTextureFromFileInMemoryEx instead of D3DXCreateTextureFromFileInMemory, so we can specify no mipmaps
		// Should prevent D3D from trying to generate mipmaps, reducing load times of non-mipped UI textures quite a bit
		return D3DXCreateTextureFromFileInMemoryEx.stdcall<HRESULT>(pDevice, pSrcData, SrcDataSize, D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, 1, 3, 0, nullptr, nullptr, ppTexture);
	}

	//
	// Scene texture replacement code
	//

	const static int D3DXCreateTextureFromFileInMemoryEx_Addr = 0x39406;
	const static int LoadXmtsetObject_Addr = 0x2E0D0;

	inline static std::filesystem::path CurrentXmtsetFilename;
	inline static const char* PrevXmtName = nullptr;

	inline static SafetyHookInline D3DXCreateTextureFromFileInMemoryEx = {};
	static HRESULT __stdcall D3DXCreateTextureFromFileInMemoryEx_dest(LPDIRECT3DDEVICE9 pDevice, void* pSrcData, UINT SrcDataSize, UINT Width, UINT Height, UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, void* pSrcInfo, PALETTEENTRY* pPalette, LPDIRECT3DTEXTURE9* ppTexture)
	{
		std::unique_ptr<uint8_t[]> newdata;

		if ((Settings::SceneTextureReplacement || Settings::SceneTextureDump) && pSrcData && SrcDataSize)
		{
			newdata = HandleTexture(&pSrcData, &SrcDataSize, CurrentXmtsetFilename, false);
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

			// TODO: try to pre-cache all replacement textures inside textures/load/[XmtFileName] here?
		}

		return LoadXmtsetObject.call<int>(XmtFileName, XmtIndex);
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
		XmtDumpPath = "textures\\dump";
		XmtLoadPath = "textures\\load";

		FileSystem = DirectoryFileCache(XmtLoadPath);

		bool ApplyUIHooks = Settings::UITextureReplacement || Settings::UITextureDump;
		bool ApplySceneHooks = Settings::SceneTextureReplacement || Settings::SceneTextureDump;

		// Scene hooks are applied through D3DXCreateTextureFromFileInMemoryEx
		// But our UI code also calls D3DXCreateTextureFromFileInMemoryEx to allow loading textures slightly faster
		// So we'll setup this hook if either are enabled
		if (ApplyUIHooks || ApplySceneHooks)
		{
			D3DXCreateTextureFromFileInMemoryEx = safetyhook::create_inline(Module::exe_ptr(D3DXCreateTextureFromFileInMemoryEx_Addr), D3DXCreateTextureFromFileInMemoryEx_dest);
		}

		if (ApplyUIHooks)
		{
			D3DXCreateTextureFromFileInMemory = safetyhook::create_inline(Module::exe_ptr(D3DXCreateTextureFromFileInMemory_Addr), D3DXCreateTextureFromFileInMemory_dest);
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
		}

		return true;
	}

	static TextureReplacement instance;
};
TextureReplacement TextureReplacement::instance;
