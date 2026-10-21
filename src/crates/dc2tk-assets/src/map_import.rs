//! An edited map scene (docs/modding/maps.md, "The glTF scene") back to the game's files: the .map rebuilt
//! from the scene's placements, pieces and points (map_scene, map_edit), and the map models whose
//! meshes changed rebuilt from the first piece placing each (models::build_map_model). What did
//! not change keeps its bytes.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use serde_json::{json, Map, Value};

use crate::from_gltf;
use crate::map_export::{find_model, options_for, ModelPack, PackType};
use crate::map_scene::{self, SceneNode};
use crate::map_transform::Trs;
use crate::mapfile;
use crate::to_gltf::TextureSearch;
use crate::{bail, json, map_edit, models, Context, Error, Result};

/// What an import changes.
#[derive(Debug, Default)]
pub struct Imported {
    /// The new .map, when it differs from the original.
    pub map: Option<Vec<u8>>,
    /// Rebuilt models that differ from the original: (model file name, bytes).
    pub models: Vec<(String, Vec<u8>)>,
    /// Models made in the scene, which no pack has: to add to the map's first model pack.
    pub new_models: Vec<NewModel>,
    /// Pictures the scene's materials show that differ from the disc's, or that it does not have.
    pub textures: Vec<TextureEdit>,
    pub notes: Vec<String>,
}

/// A picture of the scene that differs from the disc's (or is new to it).
#[derive(Debug)]
pub struct TextureEdit {
    /// The picture's name, as materials name it.
    pub name: String,
    pub png: Vec<u8>,
    /// No texture file of the disc has a picture of this name: it goes into the map's own.
    pub new: bool,
}

/// The materials the map's meshes use: every mesh under the map's root but those of nodes of role
/// `display` (its sky), which the scene only shows.
fn map_materials(doc: &Value, root: usize) -> Result<HashSet<usize>> {
    let mut found = HashSet::new();
    let mut seen = HashSet::new();
    let mut pending = vec![root];
    while let Some(at) = pending.pop() {
        let node = &doc["nodes"][at];
        if !seen.insert(at) || c2_of(node)["role"] == "display" {
            continue;
        }
        if let Some(mesh) = node["mesh"].as_u64() {
            for primitive in doc["meshes"][mesh as usize]["primitives"].as_array().into_iter().flatten() {
                if let Some(material) = primitive["material"].as_u64() {
                    found.insert(material as usize);
                }
            }
        }
        let children = from_gltf::list_of(node, "children")?;
        pending.extend(children.iter().filter_map(Value::as_u64).map(|child| child as usize));
    }
    Ok(found)
}

/// The pictures the scene's materials show: (picture name, the image's file), the name as the
/// model import takes it (the material's extras.texture, the image's name, its file's stem); only
/// the materials in `used`.
fn scene_pictures(doc: &Value, used: &HashSet<usize>) -> Result<Vec<(String, String)>> {
    let mut out: Vec<(String, String)> = Vec::new();
    for (at, material) in from_gltf::list_of(doc, "materials")?.iter().enumerate() {
        if !used.contains(&at) {
            continue;
        }
        let Some(texture) = material["pbrMetallicRoughness"]["baseColorTexture"]["index"].as_u64() else {
            continue;
        };
        let image = &doc["images"][doc["textures"][texture as usize]["source"].as_u64().unwrap_or(u64::MAX) as usize];
        let Some(uri) = image["uri"].as_str() else {
            continue;
        };
        let stem = uri.rsplit(['/', '\\']).next().unwrap_or(uri);
        let stem = stem.rsplit_once('.').map_or(stem, |(stem, _)| stem);
        let name = material["extras"]["texture"]
            .as_str()
            .or(image["name"].as_str().filter(|name| !name.is_empty()))
            .unwrap_or(stem)
            .to_string();
        if !name.is_empty() && !out.iter().any(|(known, _)| *known == name) {
            out.push((name, uri.replace("%20", " ")));
        }
    }
    Ok(out)
}

/// What the rebuilt map did with the scene's placements that is worth saying: the templates it
/// made for placements that differ from the others of theirs, and what became of the placements
/// deleted from the scene and those added.
fn placement_notes(
    original: &mapfile::MapFile,
    edit: &map_edit::SceneEdit,
    rebuilt: &mapfile::MapFile,
    slots: map_edit::Slots,
) -> Vec<String> {
    let mut notes = Vec::new();
    let known: HashSet<&str> = original.templates.iter().map(|template| template.name.as_str()).collect();
    let mut seen = HashSet::new();
    for template in &rebuilt.templates {
        if known.contains(template.name.as_str()) || !seen.insert(template.name.as_str()) {
            continue;
        }
        let users: Vec<&str> = rebuilt
            .placements
            .iter()
            .filter(|placement| placement.template == template.name)
            .map(|placement| placement.name.as_str())
            .collect();
        if !users.is_empty() {
            notes.push(format!("template {} is new, placed by {}", template.name, users.join(", ")));
        }
    }
    let kept = edit.placements.iter().filter(|placement| placement.slot.is_some()).count();
    let deleted = original.placements.len().saturating_sub(kept);
    if deleted > 0 {
        notes.push(match slots {
            map_edit::Slots::Keep => {
                format!(
                    "{deleted} placements deleted from the scene are kept hidden in their slots (scripts number them)"
                )
            }
            map_edit::Slots::Compact => format!("{deleted} placements deleted from the scene are left out"),
        });
    }
    let added = edit.placements.iter().filter(|placement| placement.slot.is_none() && !placement.preview).count();
    if added > 0 {
        notes.push(format!("{added} new placements, from slot {}", rebuilt.placements.len() - added));
    }
    notes
}

/// A model made in the scene (pieces naming a model no pack has, with meshes under them).
#[derive(Debug)]
pub struct NewModel {
    /// The file name pieces give (`s17_well-m.mds`).
    pub file: String,
    /// The info.cfg TYPE: drawn, floor and wall collision, or camera collision.
    pub kind: PackType,
    pub data: Vec<u8>,
}

/// A new model's pack type: from its pieces' kind, or else from its name (`-a` collision, `-c`
/// camera), as the disc names them.
fn new_model_kind(kind: &str, file: &str) -> PackType {
    match kind {
        "collision" => PackType::Collision,
        "camera" => PackType::Camera,
        "model" => PackType::Model,
        _ => {
            let stem = file.to_lowercase();
            let stem = stem.strip_suffix(".mds").unwrap_or(&stem);
            if stem.ends_with("-a") {
                PackType::Collision
            } else if stem.ends_with("-c") {
                PackType::Camera
            } else {
                PackType::Model
            }
        }
    }
}

fn floats(value: &Value) -> Option<Vec<f64>> {
    value.as_array().map(|items| items.iter().map(|item| item.as_f64().unwrap_or(0.0)).collect())
}

/// A glTF node's transform: translation/rotation/scale, or its matrix decomposed.
fn trs_of(node: &Value) -> Result<Option<Trs>> {
    if let Some(matrix) = floats(&node["matrix"]).filter(|matrix| matrix.len() == 16) {
        let matrix: [f64; 16] = matrix.try_into().unwrap();
        let (translation, rotation, scale) = crate::math::decompose(&matrix)?;
        return Ok(Some(Trs { translation, rotation, scale }));
    }
    let present = ["translation", "rotation", "scale"].iter().any(|key| node.get(key).is_some());
    if !present {
        return Ok(Some(Trs { translation: [0.0; 3], rotation: [0.0, 0.0, 0.0, 1.0], scale: [1.0; 3] }));
    }
    let three = |key: &str, default: [f64; 3]| -> [f64; 3] {
        floats(&node[key])
            .filter(|values| values.len() == 3)
            .map_or(default, |values| [values[0], values[1], values[2]])
    };
    let rotation = floats(&node["rotation"])
        .filter(|values| values.len() == 4)
        .map_or([0.0, 0.0, 0.0, 1.0], |q| [q[0], q[1], q[2], q[3]]);
    Ok(Some(Trs { translation: three("translation", [0.0; 3]), rotation, scale: three("scale", [1.0; 3]) }))
}

/// A node's scene tag, `extras.c2`: an object, which Blender keeps as JSON text when it could not
/// store it as a dict (Null when the node has none).
fn c2_of(node: &Value) -> Value {
    let c2 = &node["extras"]["c2"];
    match c2.as_str().and_then(|text| serde_json::from_str::<Value>(text).ok()) {
        Some(parsed) => parsed,
        None => c2.clone(),
    }
}

/// The scene tree under a glTF node; model nodes (role "mds") are left out, their indices kept in
/// `models` for the pieces they hang under.
fn tree_of(
    doc: &Value,
    index: usize,
    model_roots: &mut HashMap<usize, Vec<usize>>,
    seen: &mut HashSet<usize>,
) -> Result<SceneNode> {
    if !seen.insert(index) {
        return Err(Error::invalid(format!("glTF node {index} appears twice in the scene tree")));
    }
    let node = &doc["nodes"][index];
    let mut extras = node["extras"].clone();
    if extras["c2"].is_string() {
        extras["c2"] = c2_of(node);
    }
    let role = extras["c2"]["role"].as_str().unwrap_or("").to_string();
    let mut children = Vec::new();
    let mut models = Vec::new();
    let node_count = from_gltf::list_of(doc, "nodes")?.len();
    for child in from_gltf::list_of(node, "children")? {
        let Some(child) = child.as_u64().map(|child| child as usize).filter(|&child| child < node_count) else {
            bail!("glTF node {index}'s child {child} is not one of the glTF's {node_count} nodes");
        };
        let child_role = doc["nodes"][child]["extras"]["c2"]["role"].as_str().unwrap_or("");
        if role == "piece" && (child_role == "mds" || child_role.is_empty()) {
            models.push(child);
        } else {
            children.push(tree_of(doc, child, model_roots, seen)?);
        }
    }
    let model = if role == "piece" { extras["c2"]["model"].as_str().map(str::to_string) } else { None };
    if role == "piece" {
        model_roots.insert(index, models);
    }
    Ok(SceneNode {
        name: node["name"].as_str().unwrap_or("").to_string(),
        trs: trs_of(node)?,
        extras: if extras.is_null() { json!({}) } else { extras },
        children,
        model,
    })
}

/// Each glTF node's parent.
fn parents_of(doc: &Value) -> HashMap<usize, usize> {
    let mut found = HashMap::new();
    for (index, node) in doc["nodes"].as_array().into_iter().flatten().enumerate() {
        for child in node["children"].as_array().into_iter().flatten().filter_map(Value::as_u64) {
            found.insert(child as usize, index);
        }
    }
    found
}

/// The name of the placement a piece's node is in, for notes.
fn placement_name(doc: &Value, parents: &HashMap<usize, usize>, mut at: usize) -> String {
    while let Some(&parent) = parents.get(&at) {
        at = parent;
        if c2_of(&doc["nodes"][at])["role"] == "placement" {
            break;
        }
    }
    doc["nodes"][at]["name"].as_str().unwrap_or("").to_string()
}

/// How a piece's model nodes are laid out: their names, meshes and transforms, rounded, with a
/// rotation and its negation alike (Blender writes either), so the pieces of a model that nobody
/// moved inside compare equal.
fn model_layout(doc: &Value, roots: &[usize]) -> String {
    let mut text = String::new();
    let mut pending: Vec<usize> = roots.iter().rev().copied().collect();
    while let Some(at) = pending.pop() {
        let node = &doc["nodes"][at];
        let trs = trs_of(node).ok().flatten().unwrap_or(Trs {
            translation: [0.0; 3],
            rotation: [0.0, 0.0, 0.0, 1.0],
            scale: [1.0; 3],
        });
        let sign = if trs.rotation.iter().rev().find(|value| value.abs() > 1e-6).is_some_and(|value| *value < 0.0) {
            -1.0
        } else {
            1.0
        };
        let rounded = |values: &[f64]| {
            values
                .iter()
                .map(|value| format!("{:.3}", (value * 1000.0).round() / 1000.0 + 0.0))
                .collect::<Vec<_>>()
                .join(",")
        };
        let rotation: Vec<f64> = trs.rotation.iter().map(|value| value * sign).collect();
        text.push_str(&format!(
            "{}|{}|{}|{}|{};",
            node["name"].as_str().unwrap_or(""),
            node["mesh"],
            rounded(&trs.translation),
            rounded(&rotation),
            rounded(&trs.scale)
        ));
        let children: Vec<usize> = node["children"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_u64)
            .map(|child| child as usize)
            .collect();
        pending.extend(children.into_iter().rev());
    }
    text
}

/// The items of one of a glTF doc's lists that a subtree takes: their indices in the order first
/// reached, and each one's index in the subtree.
#[derive(Default)]
struct Taken {
    order: Vec<usize>,
    new: HashMap<usize, usize>,
}

impl Taken {
    /// Takes the item a reference (`value`, a JSON int; anything else references nothing) names in
    /// `list`; a reference past the list's end is an error naming `what` makes it.
    fn add(&mut self, value: &Value, list: &[Value], what: &str, list_name: &str) -> Result<()> {
        let Some(at) = value.as_u64() else {
            return Ok(());
        };
        let Some(at) = usize::try_from(at).ok().filter(|&at| at < list.len()) else {
            bail!("{what} {at} is not one of the glTF's {} {list_name}", list.len());
        };
        if !self.new.contains_key(&at) {
            self.new.insert(at, self.order.len());
            self.order.push(at);
        }
        Ok(())
    }

    /// The reference at `pointer` in `value` (a JSON pointer: `/mesh`, or "" for the value itself)
    /// given its subtree index.
    fn renumber(&self, value: &mut Value, pointer: &str) {
        if let Some(slot) = value.pointer_mut(pointer) {
            if let Some(&new) = slot.as_u64().and_then(|at| self.new.get(&(at as usize))) {
                *slot = json!(new);
            }
        }
    }

    /// The items taken from `list`, in their subtree order, each with its references renumbered
    /// (`fields`: the pointer to a reference, and the list it references).
    fn items(&self, list: &[Value], fields: &[(&str, &Taken)]) -> Vec<Value> {
        self.order
            .iter()
            .map(|&at| {
                let mut item = list[at].clone();
                for (pointer, taken) in fields {
                    taken.renumber(&mut item, pointer);
                }
                item
            })
            .collect()
    }
}

/// Where a material references its textures (each a texture info, `{"index": n}`).
pub(crate) const TEXTURE_FIELDS: [&str; 5] = [
    "/pbrMetallicRoughness/baseColorTexture/index",
    "/pbrMetallicRoughness/metallicRoughnessTexture/index",
    "/normalTexture/index",
    "/occlusionTexture/index",
    "/emissiveTexture/index",
];

/// The part of a glTF doc a set of root nodes reaches, renumbered as a doc of its own (nodes,
/// meshes, accessors, materials, textures, images, samplers, views and buffers). A reference to
/// something the doc does not have (a node's `"mesh": 99`) is an error.
pub fn subtree(doc: &Value, roots: &[usize]) -> Result<Value> {
    let list = |key: &str| doc[key].as_array().map(Vec::as_slice).unwrap_or_default();
    let (nodes, meshes, accessors, materials, textures) =
        (list("nodes"), list("meshes"), list("accessors"), list("materials"), list("textures"));
    let (images, samplers, views, buffers) = (list("images"), list("samplers"), list("bufferViews"), list("buffers"));

    // nodes reachable from the roots, in order
    let mut node_taken = Taken::default();
    let mut pending: Vec<Value> = roots.iter().rev().map(|&root| json!(root)).collect();
    while let Some(at) = pending.pop() {
        let before = node_taken.order.len();
        node_taken.add(&at, nodes, "a node's child", "nodes")?;
        if node_taken.order.len() == before {
            continue;
        }
        let node = &nodes[node_taken.order[before]];
        pending.extend(from_gltf::list_of(node, "children")?.iter().rev().cloned());
    }
    let mut mesh_taken = Taken::default();
    for &at in &node_taken.order {
        mesh_taken.add(&nodes[at]["mesh"], meshes, "a node's mesh", "meshes")?;
    }
    let (mut accessor_taken, mut material_taken) = (Taken::default(), Taken::default());
    for &mesh in &mesh_taken.order {
        for primitive in from_gltf::list_of(&meshes[mesh], "primitives")? {
            for value in json::object(from_gltf::item(primitive, "attributes")?, "a primitive's attributes")?.values() {
                accessor_taken.add(value, accessors, "a primitive's attribute accessor", "accessors")?;
            }
            accessor_taken.add(&primitive["indices"], accessors, "a primitive's index accessor", "accessors")?;
            material_taken.add(&primitive["material"], materials, "a primitive's material", "materials")?;
        }
    }
    let mut texture_taken = Taken::default();
    for &material in &material_taken.order {
        for pointer in TEXTURE_FIELDS {
            let index = materials[material].pointer(pointer).unwrap_or(&Value::Null);
            texture_taken.add(index, textures, "a material's texture", "textures")?;
        }
    }
    let (mut image_taken, mut sampler_taken) = (Taken::default(), Taken::default());
    for &texture in &texture_taken.order {
        image_taken.add(&textures[texture]["source"], images, "a texture's image", "images")?;
        sampler_taken.add(&textures[texture]["sampler"], samplers, "a texture's sampler", "samplers")?;
    }
    let mut view_taken = Taken::default();
    for &accessor in &accessor_taken.order {
        view_taken.add(&accessors[accessor]["bufferView"], views, "an accessor's buffer view", "buffer views")?;
    }
    for &image in &image_taken.order {
        view_taken.add(&images[image]["bufferView"], views, "an image's buffer view", "buffer views")?;
    }
    let mut buffer_taken = Taken::default();
    for &view in &view_taken.order {
        buffer_taken.add(&views[view]["buffer"], buffers, "a buffer view's buffer", "buffers")?;
    }

    let texture_fields: Vec<(&str, &Taken)> = TEXTURE_FIELDS.iter().map(|&pointer| (pointer, &texture_taken)).collect();
    let mut out = Map::new();
    out.insert("asset".into(), doc["asset"].clone());
    for (key, value) in [
        ("nodes", subtree_nodes(nodes, &node_taken, &mesh_taken)),
        ("meshes", subtree_meshes(meshes, &mesh_taken, &accessor_taken, &material_taken)),
        ("accessors", accessor_taken.items(accessors, &[("/bufferView", &view_taken)])),
        ("materials", material_taken.items(materials, &texture_fields)),
        ("textures", texture_taken.items(textures, &[("/source", &image_taken), ("/sampler", &sampler_taken)])),
        ("images", image_taken.items(images, &[("/bufferView", &view_taken)])),
        ("samplers", sampler_taken.items(samplers, &[])),
        ("bufferViews", view_taken.items(views, &[("/buffer", &buffer_taken)])),
        ("buffers", buffer_taken.items(buffers, &[])),
    ] {
        out.insert(key.into(), Value::Array(value));
    }
    for key in ["extensionsUsed", "extensionsRequired"] {
        if let Some(list) = doc[key].as_array() {
            let kept: Vec<Value> = list.iter().filter(|name| *name != "KHR_lights_punctual").cloned().collect();
            if !kept.is_empty() {
                out.insert(key.into(), Value::Array(kept));
            }
        }
    }
    let root_list: Vec<usize> = roots.iter().filter_map(|root| node_taken.new.get(root).copied()).collect();
    out.insert("scenes".into(), json!([{"nodes": root_list}]));
    out.insert("scene".into(), json!(0));
    Ok(Value::Object(out))
}

/// The nodes a subtree takes, renumbered, without what a model's own export has no use for: skins,
/// cameras and the scene's tags.
fn subtree_nodes(nodes: &[Value], node_taken: &Taken, mesh_taken: &Taken) -> Vec<Value> {
    let mut out_nodes = node_taken.items(nodes, &[("/mesh", mesh_taken)]);
    for node in &mut out_nodes {
        if let Some(object) = node.as_object_mut() {
            object.remove("skin");
            object.remove("camera");
            // the scene's own tag, which the model's export does not have
            if let Some(extras) = object.get_mut("extras").and_then(Value::as_object_mut) {
                extras.remove("c2");
            }
            // every child was taken (or refused), so each has its new number
            if let Some(children) = object.get_mut("children").and_then(Value::as_array_mut) {
                children.retain(|child| child.as_u64().is_some());
                children.iter_mut().for_each(|child| node_taken.renumber(child, ""));
            }
        }
    }
    out_nodes
}

/// The meshes a subtree takes, their primitives' accessors and materials renumbered and their
/// morph targets left out.
fn subtree_meshes(meshes: &[Value], mesh_taken: &Taken, accessor_taken: &Taken, material_taken: &Taken) -> Vec<Value> {
    let mut out_meshes = mesh_taken.items(meshes, &[]);
    for mesh in &mut out_meshes {
        for primitive in mesh.get_mut("primitives").and_then(Value::as_array_mut).into_iter().flatten() {
            if let Some(attributes) = primitive.get_mut("attributes").and_then(Value::as_object_mut) {
                attributes.values_mut().for_each(|value| accessor_taken.renumber(value, ""));
            }
            if let Some(object) = primitive.as_object_mut() {
                object.remove("targets");
            }
            accessor_taken.renumber(primitive, "/indices");
            material_taken.renumber(primitive, "/material");
        }
    }
    out_meshes
}

/// The scene's map and models against the originals. `read` gives a file of the scene's folder
/// (buffers, images) by relative path.
pub fn import_map(
    gltf: &[u8],
    read: &dyn Fn(&str) -> Result<Vec<u8>>,
    original: &[u8],
    packs: &[ModelPack],
    textures: &TextureSearch,
    slots: map_edit::Slots,
) -> Result<Imported> {
    let doc: Value = serde_json::from_slice(gltf).context("the scene's glTF")?;
    let original_map = mapfile::parse(original)?;
    let roots: Vec<usize> = doc["scenes"][doc["scene"].as_u64().unwrap_or(0) as usize]["nodes"]
        .as_array()
        .map(|nodes| nodes.iter().filter_map(Value::as_u64).map(|node| node as usize).collect())
        .unwrap_or_default();
    // the map's root: the scene root whose extras say so (or one below a wrapper Blender adds)
    let mut root_index = None;
    let mut pending = roots.clone();
    while let Some(at) = pending.pop() {
        let node = &doc["nodes"][at];
        if c2_of(node)["role"] == "map" {
            root_index = Some(at);
            break;
        }
        pending
            .extend(from_gltf::list_of(node, "children")?.iter().filter_map(Value::as_u64).map(|child| child as usize));
    }
    let Some(root_index) = root_index else {
        return Err(Error::invalid("the scene has no map root (a node whose extras.c2.role is \"map\")"));
    };
    let mut model_roots = HashMap::new();
    let tree = tree_of(&doc, root_index, &mut model_roots, &mut HashSet::new())?;

    let mut out = Imported::default();
    let edit = map_scene::read_scene(&tree, &original_map)?;
    let rebuilt = map_edit::rebuild_with(&original_map, &edit, slots)?;
    let text = mapfile::write(&rebuilt)?;
    if text != original {
        out.map = Some(text);
        out.notes.extend(placement_notes(&original_map, &edit, &rebuilt, slots));
    }

    // each model's pieces, lowest glTF node index first: the one whose node subtree differs from the
    // disc's model is the model as edited (the first when none does)
    let mut piece_nodes: Vec<usize> = model_roots.keys().copied().collect();
    piece_nodes.sort();
    let mut pieces_of: HashMap<String, Vec<usize>> = HashMap::new();
    for &piece in &piece_nodes {
        if let Some(model) = c2_of(&doc["nodes"][piece])["model"].as_str() {
            pieces_of.entry(model.to_lowercase()).or_default().push(piece);
        }
    }
    let parents = parents_of(&doc);
    let mut done: HashSet<String> = HashSet::new();
    // the scene's buffers, read once though every model's subtree names them
    let buffer_cache: RefCell<HashMap<String, Arc<Vec<u8>>>> = RefCell::new(HashMap::new());
    let read_cached = |path: &str| -> Result<Arc<Vec<u8>>> {
        if let Some(data) = buffer_cache.borrow().get(path) {
            return Ok(Arc::clone(data));
        }
        let data = Arc::new(read(path)?);
        buffer_cache.borrow_mut().insert(path.to_string(), Arc::clone(&data));
        Ok(data)
    };
    for piece in piece_nodes {
        let c2 = c2_of(&doc["nodes"][piece]);
        let Some(model) = c2["model"].as_str().map(str::to_string) else {
            continue;
        };
        let model_roots_here = &model_roots[&piece];
        if model.is_empty() || model_roots_here.is_empty() || !done.insert(model.to_lowercase()) {
            continue;
        }
        let Some((kind, data)) = find_model(packs, &model) else {
            let sub = subtree(&doc, model_roots_here).context(&model)?;
            let given = from_gltf::with_buffers(sub, &read_cached)?;
            // made in the scene: a new model of every mesh under its first piece
            let kind = new_model_kind(c2["kind"].as_str().unwrap_or(""), &model);
            let stem = model.rsplit_once('.').map_or(model.as_str(), |(stem, _)| stem);
            let data =
                models::new_map_model(&given, stem, &options_for(kind, textures)).with_context(|| model.clone())?;
            out.new_models.push(NewModel { file: model, kind, data });
            continue;
        };
        if kind == PackType::Character {
            continue;
        }
        let mut layouts: Vec<(String, usize)> = Vec::new();
        for &other in &pieces_of[&model.to_lowercase()] {
            let layout = model_layout(&doc, &model_roots[&other]);
            if !layouts.iter().any(|(seen, _)| *seen == layout) {
                layouts.push((layout, other));
            }
        }
        let mut edited: Vec<(usize, Vec<u8>)> = Vec::new();
        for (_, first) in &layouts {
            let sub = subtree(&doc, &model_roots[first]).context(&model)?;
            let given = from_gltf::with_buffers(sub, &read_cached)?;
            let rebuilt =
                models::build_map_model(&given, data, &options_for(kind, textures)).with_context(|| model.clone())?;
            if rebuilt != data {
                edited.push((*first, rebuilt));
            }
        }
        if edited.len() > 1 {
            let named: Vec<String> = edited.iter().map(|(piece, _)| placement_name(&doc, &parents, *piece)).collect();
            out.notes.push(format!(
                "model {model} is edited differently in placements {}; every placement shares the model, so the build \
                 takes the edit in {}",
                named.join(", "),
                named[0]
            ));
        }
        if let Some((_, rebuilt)) = edited.into_iter().next() {
            out.models.push((model, rebuilt));
        }
    }
    let used = map_materials(&doc, root_index)?;
    for (name, uri) in scene_pictures(&doc, &used)? {
        let png = read_cached(&uri).with_context(|| format!("picture {name}"))?.to_vec();
        match textures.find(&name) {
            // the first of the name is a render target, which the export showed as its stand-in
            // (d01f01's d01b17, which d01b01 has with pixels)
            Some((_, first))
                if first.no_image && crate::textures::same_pixels(&png, &crate::to_gltf::placeholder_png(&name))? => {}
            Some(_) => {
                // changed when it is none of the disc's pictures of the name (several maps have
                // their own under one name; a render target has no pixels to change)
                let pictures = textures.pngs_named(&name)?;
                let mut unchanged = pictures.is_empty();
                for old in &pictures {
                    unchanged = unchanged || crate::textures::same_pixels(old, &png)?;
                }
                if !unchanged {
                    out.textures.push(TextureEdit { name, png, new: false });
                }
            }
            // the exporter's stand-in for a picture the disc lacks is not a picture to add
            None if crate::textures::same_pixels(&png, &crate::to_gltf::placeholder_png(&name))? => {}
            None => out.textures.push(TextureEdit { name, png, new: true }),
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::map_export::export_map;

    #[test]
    fn a_subtree_renumbers_what_its_roots_reach_and_refuses_what_the_gltf_lacks() {
        let doc = json!({
            "asset": {"version": "2.0"},
            "nodes": [{"name": "other", "mesh": 1}, {"name": "piece", "children": [2]}, {"name": "m", "mesh": 1}],
            "meshes": [
                {"primitives": [{"attributes": {"POSITION": 0}}]},
                {"primitives": [{"attributes": {"POSITION": 1}, "indices": 0, "material": 0}]},
            ],
            "accessors": [{"bufferView": 0}, {"bufferView": 1}],
            "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
            "textures": [{"source": 0}],
            "images": [{"uri": "a.png"}],
            "bufferViews": [{"buffer": 0}, {"buffer": 0}],
            "buffers": [{"uri": "x.bin"}],
        });
        let sub = subtree(&doc, &[1]).unwrap();
        assert_eq!(sub["nodes"].as_array().unwrap().len(), 2);
        assert_eq!((&sub["nodes"][0]["children"], &sub["nodes"][1]["mesh"]), (&json!([1]), &json!(0)));
        // the mesh's accessors in the order first reached: its position, then its indices
        assert_eq!(
            sub["meshes"][0]["primitives"][0],
            json!({"attributes": {"POSITION": 0}, "indices": 1, "material": 0})
        );
        assert_eq!(sub["accessors"], json!([{"bufferView": 0}, {"bufferView": 1}]));
        assert_eq!(sub["scenes"], json!([{"nodes": [0]}]));
        // a mesh, material and child the glTF does not have
        for (pointer, value, error) in [
            ("/nodes/2/mesh", 99, "a node's mesh 99 is not one of the glTF's 2 meshes"),
            ("/meshes/1/primitives/0/material", 5, "a primitive's material 5 is not one of the glTF's 1 materials"),
            ("/nodes/1/children/0", 7, "a node's child 7 is not one of the glTF's 3 nodes"),
        ] {
            let mut bad = doc.clone();
            *bad.pointer_mut(pointer).unwrap() = json!(value);
            assert_eq!(subtree(&bad, &[1]).unwrap_err().to_string(), error);
        }
    }

    #[test]
    fn a_scene_of_the_wrong_json_shape_is_an_error_not_read_as_nothing() {
        let doc = json!({
            "nodes": [{"name": "map", "children": [1], "mesh": 0}, {"name": "piece"}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
            "accessors": [{}],
        });
        assert!(subtree(&doc, &[0]).is_ok());
        for (pointer, value, error) in [
            ("/nodes/1", json!({"children": 2}), "the glTF has the number 2 where it needs a list of \"children\""),
            ("/meshes/0/primitives", json!({}), "the glTF has an object where it needs a list of \"primitives\""),
            ("/meshes/0/primitives/0/attributes", json!([0]), "a primitive's attributes is an object, not a list"),
        ] {
            let mut bad = doc.clone();
            *bad.pointer_mut(pointer).unwrap() = value;
            assert_eq!(subtree(&bad, &[0]).unwrap_err().to_string(), error);
        }
        // a scene node's child past the glTF's nodes, which the tree read took as nothing
        let bad = json!({"nodes": [{"name": "map", "children": [5]}]});
        let error = tree_of(&bad, 0, &mut HashMap::new(), &mut HashSet::new()).err().unwrap().to_string();
        assert_eq!(error, "glTF node 0's child 5 is not one of the glTF's 1 nodes");
        let bad = json!({"materials": {"name": "x"}});
        assert_eq!(
            scene_pictures(&bad, &HashSet::new()).unwrap_err().to_string(),
            "the glTF has an object where it needs a list of \"materials\""
        );
    }

    /// Every fixed map of an extracted disc (DC2TK_CORPUS) exported and imported unedited changes
    /// nothing: the same .map, no model rebuilt.
    #[test]
    #[ignore = "needs DC2TK_CORPUS"]
    fn corpus_unedited_scenes_change_nothing() {
        let corpus = dc2tk_disc::samples::corpus();
        let root = std::path::Path::new(&corpus);
        let mut checked = 0;
        let mut shared_checked = 0;
        for name in ["map/s/s17/s17", "map/m/m05/m05", "map/m/m01/m01"] {
            let folder = root.join(name).parent().unwrap().to_path_buf();
            let stem = name.rsplit('/').next().unwrap();
            let map_bytes = std::fs::read(root.join(format!("{name}.map"))).unwrap();
            let map = mapfile::parse(&map_bytes).unwrap();
            let mut packs = Vec::new();
            let mpk = folder.join(format!("{stem}.mpk.d"));
            for entry in std::fs::read_dir(&mpk).unwrap().flatten() {
                let path = entry.path();
                if path.is_dir() {
                    let members = std::fs::read_dir(&path)
                        .unwrap()
                        .flatten()
                        .filter(|member| member.path().is_file())
                        .map(|member| {
                            (member.file_name().to_string_lossy().into_owned(), std::fs::read(member.path()).unwrap())
                        })
                        .collect();
                    let pack_name = path.file_name().unwrap().to_string_lossy().trim_end_matches(".d").to_string();
                    packs.push(ModelPack::new(&pack_name, members));
                }
            }
            let ipk = folder.join(format!("{stem}.ipk.d"));
            let images: Vec<(String, Vec<u8>)> = std::fs::read_dir(&ipk)
                .unwrap()
                .flatten()
                .filter(|entry| entry.path().extension().is_some_and(|ext| ext == "img"))
                .map(|entry| (entry.file_name().to_string_lossy().into_owned(), std::fs::read(entry.path()).unwrap()))
                .collect();
            let search = TextureSearch::new(&images).unwrap();
            // with the sky and the .cfg the scene shows, which change nothing either
            let sky_dir = root.join("map/def.sky.d");
            let sky = std::fs::read_dir(&sky_dir)
                .unwrap()
                .flatten()
                .map(|entry| (entry.file_name().to_string_lossy().into_owned(), std::fs::read(entry.path()).unwrap()))
                .collect();
            let cfg = std::fs::read(folder.join(format!("{stem}.cfg"))).ok();
            let display = crate::map_export::Display { sky, cfg };
            let (files, _) = export_map(stem, &map, &packs, &search, &display).unwrap();
            let gltf_doc: Value = serde_json::from_slice(&files[&format!("{stem}.gltf")]).unwrap();
            let display_nodes =
                gltf_doc["nodes"].as_array().unwrap().iter().filter(|node| c2_of(node)["role"] == "display").count();
            assert!(display_nodes > 0, "{name}: the sky is in the scene");
            let gltf = files[&format!("{stem}.gltf")].clone();
            let read = |path: &str| files.get(path).cloned().ok_or_else(|| Error::invalid(format!("no {path}")));
            let imported = import_map(&gltf, &read, &map_bytes, &packs, &search, map_edit::Slots::Keep).unwrap();
            assert!(imported.map.is_none(), "{name}: the .map changed");
            let changed: Vec<&String> = imported.models.iter().map(|(model, _)| model).collect();
            assert!(changed.is_empty(), "{name}: models rebuilt: {changed:?}");
            assert!(imported.new_models.is_empty(), "{name}: new models");
            let pictures: Vec<&String> = imported.textures.iter().map(|edit| &edit.name).collect();
            assert!(pictures.is_empty(), "{name}: pictures changed: {pictures:?}");
            let nodes = gltf_doc["nodes"].as_array().unwrap();
            let mut pieces: HashMap<String, Vec<usize>> = HashMap::new();
            for (index, node) in nodes.iter().enumerate() {
                let c2 = c2_of(node);
                let has_model_node =
                    node["children"].as_array().into_iter().flatten().filter_map(Value::as_u64).any(|child| {
                        matches!(nodes[child as usize]["extras"]["c2"]["role"].as_str(), Some("mds") | None)
                    });
                if c2["role"] == "piece" && has_model_node {
                    pieces.entry(c2["model"].as_str().unwrap_or("").to_lowercase()).or_default().push(index);
                }
            }
            let Some((model, shared)) =
                pieces.iter().filter(|(_, found)| found.len() > 1).min_by_key(|(model, _)| *model)
            else {
                checked += 1;
                continue;
            };
            let model_node = |piece: usize| {
                let mut at = piece;
                for _ in 0..2 {
                    match nodes[at]["children"].as_array().and_then(|children| children.first()).and_then(Value::as_u64)
                    {
                        Some(child) => at = child as usize,
                        None => break,
                    }
                }
                at
            };
            let mut moved: Value = gltf_doc.clone();
            moved["nodes"][model_node(shared[1])]["translation"] = json!([5.0, 0.0, 0.0]);
            let text = serde_json::to_vec(&moved).unwrap();
            let imported = import_map(&text, &read, &map_bytes, &packs, &search, map_edit::Slots::Keep).unwrap();
            let changed: Vec<String> = imported.models.iter().map(|(found, _)| found.to_lowercase()).collect();
            assert_eq!(
                changed,
                std::slice::from_ref(model),
                "{name}: a model moved inside its second piece is rebuilt"
            );
            moved["nodes"][model_node(shared[0])]["translation"] = json!([0.0, 0.0, 7.0]);
            let text = serde_json::to_vec(&moved).unwrap();
            let imported = import_map(&text, &read, &map_bytes, &packs, &search, map_edit::Slots::Keep).unwrap();
            assert_eq!(imported.models.len(), 1, "{name}: one model, however many pieces show it");
            assert!(
                imported.notes.iter().any(|note| note.contains("edited differently")),
                "{name}: {:?}",
                imported.notes
            );
            shared_checked += 1;
            checked += 1;
        }
        assert!(shared_checked > 0, "no corpus map has a model two pieces share");
        println!("{checked} maps export and import unchanged");
    }
}
