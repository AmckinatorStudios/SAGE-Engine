#!/usr/bin/env python3
# ============================================================================
#  Генератор тестовой скелетной модели .glb.
#
#  Зачем он в репозитории. Чтобы измерять время загрузки и проверять кэш
#  ресурсов, нужна НАСТОЯЩАЯ модель: со скином, костями, клипами и встроенной
#  текстурой. Класть в репозиторий чужой ассет — значит тащить лицензию и
#  мегабайты; генератор занимает несколько килобайт и даёт модель ЛЮБОГО
#  размера, а значит позволяет мерить зависимость времени от объёма.
#
#  Результат — обычный glTF 2.0 без расширений: его читает и движок, и Blender.
#
#  Запуск:
#    python3 tools/make_test_model.py out.glb --segments 64 --rings 64 --texture 512
# ============================================================================
import argparse
import json
import math
import struct
import zlib


def png_bytes(size: int) -> bytes:
    """Клетчатая текстура PNG size x size. Своя, а не из файла: генератор не
    должен зависеть ни от каких внешних данных."""
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # фильтр строки
        for x in range(size):
            checker = ((x >> 4) + (y >> 4)) & 1
            r = 210 if checker else 60
            g = 140 if checker else 90
            b = 70 if checker else 150
            rows += bytes((r, g, b))

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)
    signature = b'\x89PNG\r\n\x1a\n'
    return (signature + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))


def build(segments: int, rings: int, bones: int, clips: int, texture: int):
    # --- Геометрия: труба вдоль оси Y, скинованная на цепочку костей ---
    positions, normals, uvs, joints, weights = [], [], [], [], []
    height = 6.0
    for s in range(segments + 1):
        v = s / segments
        y = v * height
        # Кость выбирается по высоте; вес размазан между соседними, иначе на
        # границе сегмента модель ломалась бы углом.
        bone = min(int(v * (bones - 1)), bones - 2)
        local = v * (bones - 1) - bone
        for r in range(rings):
            a = 2.0 * math.pi * r / rings
            radius = 0.6 + 0.25 * math.sin(v * math.pi * 3.0)
            nx, nz = math.cos(a), math.sin(a)
            positions += [nx * radius, y, nz * radius]
            normals += [nx, 0.0, nz]
            uvs += [r / rings, v]
            joints += [bone, bone + 1, 0, 0]
            weights += [1.0 - local, local, 0.0, 0.0]

    indices = []
    for s in range(segments):
        for r in range(rings):
            a = s * rings + r
            b = s * rings + (r + 1) % rings
            c = (s + 1) * rings + r
            d = (s + 1) * rings + (r + 1) % rings
            indices += [a, c, b, b, c, d]

    return positions, normals, uvs, joints, weights, indices


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--segments', type=int, default=64)
    ap.add_argument('--rings', type=int, default=64)
    ap.add_argument('--bones', type=int, default=12)
    ap.add_argument('--clips', type=int, default=3)
    ap.add_argument('--texture', type=int, default=512)
    args = ap.parse_args()

    positions, normals, uvs, joints, weights, indices = build(
        args.segments, args.rings, args.bones, args.clips, args.texture)
    vertex_count = len(positions) // 3

    blob = bytearray()
    views, accessors = [], []

    def add(data: bytes, target=None):
        while len(blob) % 4:
            blob.append(0)
        offset = len(blob)
        blob.extend(data)
        view = {'buffer': 0, 'byteOffset': offset, 'byteLength': len(data)}
        if target:
            view['target'] = target
        views.append(view)
        return len(views) - 1

    def accessor(view, comp_type, count, type_, mn=None, mx=None):
        acc = {'bufferView': view, 'componentType': comp_type, 'count': count, 'type': type_}
        if mn is not None:
            acc['min'], acc['max'] = mn, mx
        accessors.append(acc)
        return len(accessors) - 1

    FLOAT, USHORT, UINT = 5126, 5123, 5125
    pos_acc = accessor(add(struct.pack(f'<{len(positions)}f', *positions), 34962), FLOAT,
                       vertex_count, 'VEC3',
                       [min(positions[0::3]), min(positions[1::3]), min(positions[2::3])],
                       [max(positions[0::3]), max(positions[1::3]), max(positions[2::3])])
    nrm_acc = accessor(add(struct.pack(f'<{len(normals)}f', *normals), 34962), FLOAT,
                       vertex_count, 'VEC3')
    uv_acc = accessor(add(struct.pack(f'<{len(uvs)}f', *uvs), 34962), FLOAT, vertex_count, 'VEC2')
    jnt_acc = accessor(add(struct.pack(f'<{len(joints)}H', *joints), 34962), USHORT,
                       vertex_count, 'VEC4')
    wgt_acc = accessor(add(struct.pack(f'<{len(weights)}f', *weights), 34962), FLOAT,
                       vertex_count, 'VEC4')
    idx_acc = accessor(add(struct.pack(f'<{len(indices)}I', *indices), 34963), UINT,
                       len(indices), 'SCALAR')

    # --- Скелет: цепочка костей вдоль Y ---
    step = 6.0 / (args.bones - 1)
    nodes = []
    joint_nodes = []
    for i in range(args.bones):
        node = {'name': f'bone{i}', 'translation': [0.0, step if i else 0.0, 0.0]}
        if i + 1 < args.bones:
            node['children'] = [i + 1]
        nodes.append(node)
        joint_nodes.append(i)

    inverse_bind = []
    for i in range(args.bones):
        y = -i * step
        inverse_bind += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, y, 0, 1]
    ibm_acc = accessor(add(struct.pack(f'<{len(inverse_bind)}f', *[float(v) for v in inverse_bind])),
                       FLOAT, args.bones, 'MAT4')

    mesh_node = len(nodes)
    nodes.append({'name': 'Body', 'mesh': 0, 'skin': 0})

    # --- Клипы: изгиб цепочки ---
    animations = []
    times = [i * 0.1 for i in range(21)]
    time_acc = accessor(add(struct.pack(f'<{len(times)}f', *times)), FLOAT, len(times), 'SCALAR',
                        [times[0]], [times[-1]])
    for c in range(args.clips):
        channels, samplers = [], []
        for b in range(1, args.bones):
            quats = []
            for t in times:
                angle = math.sin(t * 2.0 + b * 0.4 + c) * 0.25
                quats += [math.sin(angle * 0.5), 0.0, 0.0, math.cos(angle * 0.5)]
            acc = accessor(add(struct.pack(f'<{len(quats)}f', *quats)), FLOAT, len(times), 'VEC4')
            samplers.append({'input': time_acc, 'output': acc, 'interpolation': 'LINEAR'})
            channels.append({'sampler': len(samplers) - 1,
                             'target': {'node': b, 'path': 'rotation'}})
        animations.append({'name': f'Clip_{c}', 'samplers': samplers, 'channels': channels})

    png = png_bytes(args.texture)
    img_view = add(png)

    root = {
        'asset': {'version': '2.0', 'generator': 'SAGE test model generator'},
        'scene': 0,
        'scenes': [{'nodes': [0, mesh_node]}],
        'nodes': nodes,
        'meshes': [{'name': 'Body', 'primitives': [{
            'attributes': {'POSITION': pos_acc, 'NORMAL': nrm_acc, 'TEXCOORD_0': uv_acc,
                           'JOINTS_0': jnt_acc, 'WEIGHTS_0': wgt_acc},
            'indices': idx_acc, 'material': 0}]}],
        'skins': [{'inverseBindMatrices': ibm_acc, 'joints': joint_nodes, 'skeleton': 0}],
        'materials': [{'name': 'Skin', 'pbrMetallicRoughness': {
            'baseColorTexture': {'index': 0}, 'metallicFactor': 0.1, 'roughnessFactor': 0.7}}],
        'textures': [{'source': 0}],
        'images': [{'bufferView': img_view, 'mimeType': 'image/png'}],
        'animations': animations,
        'bufferViews': views,
        'accessors': accessors,
        'buffers': [{'byteLength': len(blob)}],
    }

    text = json.dumps(root, separators=(',', ':')).encode()
    while len(text) % 4:
        text += b' '
    while len(blob) % 4:
        blob.append(0)

    with open(args.out, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob)))
        f.write(struct.pack('<II', len(text), 0x4E4F534A))
        f.write(text)
        f.write(struct.pack('<II', len(blob), 0x004E4942))
        f.write(blob)

    print(f'{args.out}: вершин {vertex_count}, треугольников {len(indices) // 3}, '
          f'костей {args.bones}, клипов {args.clips}, текстура {args.texture}x{args.texture}')


if __name__ == '__main__':
    main()
