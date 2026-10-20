#include "automap.hpp"
#include "scenesnd.hpp"
#include <cassert>
#include <cstring>

mgCTextureManager::mgCTextureManager() {}
mgCTextureBlock::mgCTextureBlock() {}
void mgCMemory::Init() {}

mgCTexture *mgCTextureManager::GetTexture(char *name, int block) {
    assert(!std::strcmp(name, "minimap1") && block == -1);
    return nullptr;
}

mgCTextureManager mgTexManager;
DNG_BATTLE_AREA *BattleAreaScene;
alignas(CMapParts) static unsigned char part_storage[sizeof(CMapParts) * 4];

CMapParts *CMap::GetPlacPartsTable(int *maximum) {
    *maximum = 4;
    return reinterpret_cast<CMapParts *>(part_storage);
}

int main() {
    alignas(CMap) unsigned char map_storage[sizeof(CMap)] = {};
    alignas(CMiniMapSymbol) unsigned char symbol_storage[sizeof(CMiniMapSymbol)] = {};
    alignas(DNG_BATTLE_AREA) unsigned char area_storage[sizeof(DNG_BATTLE_AREA)] = {};
    CMap *map = reinterpret_cast<CMap *>(map_storage);
    CMiniMapSymbol *symbol = reinterpret_cast<CMiniMapSymbol *>(symbol_storage);
    BattleAreaScene = reinterpret_cast<DNG_BATTLE_AREA *>(area_storage);
    std::strcpy(BattleAreaScene->map_name, "d01f01");
    CMapParts *parts = reinterpret_cast<CMapParts *>(part_storage);
    std::strcpy(parts[0].name, "known");
    std::strcpy(parts[0].parts_name, "room08");
    std::strcpy(parts[1].name, "last");
    std::strcpy(parts[1].parts_name, "door99");
    std::strcpy(parts[2].name, "unknown");
    std::strcpy(parts[2].parts_name, "validation_unknown");
    CAutoMapParts grid[1] = {};
    symbol->SetMapInfo(map, grid, 1, 1, 320, 320);
    assert(symbol->parts_num == 3 && symbol->info == &MiniMapInfoData[0]);
    assert(parts[0].minimap_tile == 32);
    assert(parts[1].minimap_tile == 93);
    assert(parts[2].minimap_tile == -1);
    char *sentinel = PartsInfoData[AUTOMAP_PARTS_INFO_NUM - 1].name;
    assert(sentinel && !sentinel[0]);
    PartsInfoData[AUTOMAP_PARTS_INFO_NUM - 1].name = const_cast<char *>("validation_sentinel");
    symbol->SetMapInfo(map, grid, 1, 1, 320, 320);
    PartsInfoData[AUTOMAP_PARTS_INFO_NUM - 1].name = sentinel;
    assert(parts[0].minimap_tile == 32);
    assert(parts[1].minimap_tile == 93);
    assert(parts[2].minimap_tile == -1);
}
