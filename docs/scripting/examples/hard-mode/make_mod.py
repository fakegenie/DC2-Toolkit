import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
TABLES = ROOT / "extracted" / "data"
MOD = ROOT / "mods" / "hard-mode"

MONSTER_LIFE = 1.5
MONSTER_ATTACK = 1.4
WEAPON_WHP = 0.6
SHOP_PRICES = 1.5


def table(name):
    return json.loads((TABLES / f"{name}.json").read_text())


def scaled(value, factor, lowest, highest):
    if value <= 0:
        return value
    return max(lowest, min(highest, round(value * factor)))


def monsters():
    records = []
    for monster in table("monsters")["monsters"]:
        life = scaled(monster["hp"], MONSTER_LIFE, 1, 2**31 - 1)
        attack = scaled(monster["attack"], MONSTER_ATTACK, 1, 32767)
        if (life, attack) != (monster["hp"], monster["attack"]):
            records.append({"id": monster["id"], "hp": life, "attack": attack})
    return {"monsters": records}


def items():
    records = []
    for item in table("items")["items"]:
        whp = item.get("weapon", {}).get("whp", 0)
        lower = scaled(whp, WEAPON_WHP, 1, 255)
        if lower != whp:
            records.append({"item": item["item"], "weapon": {"whp": lower}})
    return {"items": records}


def shops():
    records = []
    for price in table("shops")["prices"]:
        buy = price["buy"]
        if isinstance(buy, list):
            buy = [scaled(value, SHOP_PRICES, 1, 2**31 - 1) for value in buy]
        else:
            buy = scaled(buy, SHOP_PRICES, 1, 2**31 - 1)
        if buy != price["buy"]:
            records.append({"item": price["item"], "buy": buy})
    return {"prices": records}


def write(name, body):
    path = MOD / "data" / f"{name}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(body, indent=1) + "\n")
    print(f"{path.relative_to(ROOT)}: {len(next(iter(body.values())))} records")


def main():
    if not TABLES.is_dir():
        sys.exit("extracted/data is missing: run dc2tk setup first")
    MOD.mkdir(parents=True, exist_ok=True)
    shutil.copy(HERE / "mod.json", MOD / "mod.json")
    shutil.copy(HERE / "README.md", MOD / "README.md")
    shutil.copytree(HERE / "sets", MOD / "sets", dirs_exist_ok=True)
    write("monsters", monsters())
    write("items", items())
    write("shops", shops())


main()
