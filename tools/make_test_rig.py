#!/usr/bin/env python3
# ============================================================================
#  Генератор ПРОВЕРОЧНОЙ ОСНАСТКИ test_rig.glb — модели, собранной специально
#  под то, на чём скелетный рендер ломался.
#
#  Зачем отдельно от make_test_model.py. Тот делает БОЛЬШУЮ модель и нужен,
#  чтобы мерить время загрузки и кэш: труба, скин, клипы, крупная текстура.
#  Здесь наоборот — модель на несколько килобайт, но в ней собрано ровно то,
#  что встречается в настоящих персонажах и что движок терял:
#
#   1. ЖЁСТКАЯ ДЕТАЛЬ НА КОСТИ — меш без весов, подвешенный к кости узлом. Так
#      риггят зубы, глаза, панели, болты: это дешевле и точнее весов. Разбор,
#      который отбирал примитивы по наличию JOINTS_0, выбрасывал их молча — у
#      проверочного Springtrap так пропадало 161 из 179 частей.
#   2. УПАКОВАННАЯ КАРТА metallic-roughness — одна текстура, где G это
#      шероховатость, B металличность. Множитель metallicFactor у неё 1.0:
#      модель, которая читает только множитель и не читает карту, становится
#      сплошным металлом, перестаёт принимать рассеянный свет и выглядит
#      тёмной и «бледной» одновременно. Ровно с этой жалобой и пришли.
#   3. СВЕЧЕНИЕ — emissiveFactor у детали, чтобы проверять, что оно доезжает.
#   4. ПОВОРОТ НАД СКЕЛЕТОМ — узел с матрицей выше корневой кости. По
#      спецификации обратные bind-матрицы заданы от корня СЦЕНЫ, и цепочка над
#      скелетом обязана учитываться, иначе модель выходит развёрнутой.
#   5. ОТСЕЧЕНИЕ ПО АЛЬФЕ и ДВУСТОРОННИЙ материал (alphaMode=MASK).
#
#  Запуск:
#    python3 tools/make_test_rig.py engine/assets/test_rig.glb
# ============================================================================
import argparse
import json
import math
import struct
import zlib


def png_rgba(width: int, height: int, texel) -> bytes:
    """PNG RGBA width x height; texel(x, y) -> (r, g, b, a)."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # фильтр строки
        for x in range(width):
            rows += bytes(texel(x, y))

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)  # 6 = RGBA
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))


def box(sx: float, sy: float, sz: float, oy: float = 0.0):
    """Коробка с центром в (0, oy, 0). Нормали по граням, развёртка простая."""
    hx, hy, hz = sx * 0.5, sy * 0.5, sz * 0.5
    faces = [
        ((0, 0, 1), [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
        ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),
        ((1, 0, 0), [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
        ((-1, 0, 0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
        ((0, 1, 0), [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ((0, -1, 0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
    ]
    pos, nrm, uv, idx = [], [], [], []
    for normal, corners in faces:
        base = len(pos) // 3
        for i, (x, y, z) in enumerate(corners):
            pos += [x, y + oy, z]
            nrm += list(normal)
            uv += [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]][i]
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, uv, idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    args = ap.parse_args()

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

    def floats(values, comps, target=None, bounds=False):
        view = add(struct.pack(f'<{len(values)}f', *[float(v) for v in values]), target)
        mn = mx = None
        if bounds:
            mn = [min(values[c::comps]) for c in range(comps)]
            mx = [max(values[c::comps]) for c in range(comps)]
        kind = {1: 'SCALAR', 2: 'VEC2', 3: 'VEC3', 4: 'VEC4', 16: 'MAT4'}[comps]
        return accessor(view, FLOAT, len(values) // comps, kind, mn, mx)

    # --- 1. Скиновое тело: столбик, низ на кости 0, верх на кости 1 ----------
    bpos, bnrm, buv, bidx = box(1.0, 2.0, 1.0, oy=1.0)
    joints, weights = [], []
    for i in range(len(bpos) // 3):
        top = bpos[i * 3 + 1] > 1.0     # верхняя половина — вторая кость
        joints += [1 if top else 0, 0, 0, 0]
        weights += [1.0, 0.0, 0.0, 0.0]
    body = {
        'POSITION': floats(bpos, 3, 34962, bounds=True),
        'NORMAL': floats(bnrm, 3, 34962),
        'TEXCOORD_0': floats(buv, 2, 34962),
        'JOINTS_0': accessor(add(struct.pack(f'<{len(joints)}H', *joints), 34962), USHORT,
                             len(joints) // 4, 'VEC4'),
        'WEIGHTS_0': floats(weights, 4, 34962),
    }
    body_idx = accessor(add(struct.pack(f'<{len(bidx)}I', *bidx), 34963), UINT, len(bidx), 'SCALAR')

    # --- 2. Жёсткая деталь: кубик БЕЗ весов, висит на второй кости ----------
    rpos, rnrm, ruv, ridx = box(0.7, 0.7, 0.7)
    rigid = {
        'POSITION': floats(rpos, 3, 34962, bounds=True),
        'NORMAL': floats(rnrm, 3, 34962),
        'TEXCOORD_0': floats(ruv, 2, 34962),
    }
    rigid_idx = accessor(add(struct.pack(f'<{len(ridx)}I', *ridx), 34963), UINT, len(ridx), 'SCALAR')

    # --- 3. Двусторонняя пластинка с отсечением по альфе --------------------
    ppos = [-0.9, 0.0, 0.0, 0.9, 0.0, 0.0, 0.9, 1.2, 0.0, -0.9, 1.2, 0.0]
    pnrm = [0, 0, 1] * 4
    puv = [0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0]
    pidx = [0, 1, 2, 0, 2, 3]
    plate = {
        'POSITION': floats(ppos, 3, 34962, bounds=True),
        'NORMAL': floats(pnrm, 3, 34962),
        'TEXCOORD_0': floats(puv, 2, 34962),
    }
    plate_idx = accessor(add(struct.pack(f'<{len(pidx)}I', *pidx), 34963), UINT, len(pidx), 'SCALAR')

    # --- Скелет: две кости, обратные bind-матрицы от КОРНЯ СЦЕНЫ ------------
    #
    # Над скелетом стоит узел с поворотом на 180° вокруг Y, и bind-матрицы
    # учитывают его — как это делают настоящие экспортёры. Разбор, который
    # цепочку над скелетом отбрасывает, соберёт модель развёрнутой.
    #   world(bone0) = Rot180 ; world(bone1) = Rot180 * T(0,2,0)
    #   inverse(world) = T(0,-2,0) * Rot180^-1
    rot180 = [-1, 0, 0, 0,  0, 1, 0, 0,  0, 0, -1, 0,  0, 0, 0, 1]   # по столбцам
    ibm = [
        # inverse(Rot180) = Rot180
        -1, 0, 0, 0,  0, 1, 0, 0,  0, 0, -1, 0,  0, 0, 0, 1,
        # inverse(Rot180 * T(0,2,0)) = T(0,-2,0) * Rot180
        -1, 0, 0, 0,  0, 1, 0, 0,  0, 0, -1, 0,  0, -2, 0, 1,
    ]
    ibm_acc = floats(ibm, 16)

    # --- Узлы ----------------------------------------------------------------
    nodes = [
        {'name': 'SceneRoot', 'matrix': [float(v) for v in rot180], 'children': [1, 4]},
        {'name': 'bone0', 'children': [2]},
        {'name': 'bone1', 'translation': [0.0, 2.0, 0.0], 'children': [3, 5]},
        {'name': 'RigidPart', 'mesh': 1, 'translation': [0.0, 0.6, 0.0]},
        {'name': 'Body', 'mesh': 0, 'skin': 0},
        {'name': 'MaskPlate', 'mesh': 2, 'translation': [0.0, 0.2, 0.7]},
    ]

    # --- Клип: вторая кость поворачивается вокруг Z -------------------------
    times = [0.0, 0.5, 1.0]
    time_acc = floats(times, 1, bounds=True)
    quats = []
    for t in times:
        angle = t * 0.6          # до ~34° — заметно и на глаз, и по пикселям
        quats += [0.0, 0.0, math.sin(angle * 0.5), math.cos(angle * 0.5)]
    rot_acc = floats(quats, 4)
    animations = [{'name': 'Bend',
                   'samplers': [{'input': time_acc, 'output': rot_acc, 'interpolation': 'LINEAR'}],
                   'channels': [{'sampler': 0, 'target': {'node': 2, 'path': 'rotation'}}]}]

    # --- Текстуры ------------------------------------------------------------
    # Упакованная карта: R — затенение (нейтральное), G — шероховатость 0.4,
    # B — металличность 0. Множитель металличности у материала при этом 1.0:
    # кто читает только множитель, получит сплошной металл.
    mr_png = png_rgba(4, 4, lambda x, y: (255, 102, 0, 255))
    # Карта с дырками: половина текселей прозрачная — для alphaMode=MASK.
    mask_png = png_rgba(8, 8, lambda x, y: (220, 220, 220, 0 if (x + y) % 2 else 255))
    # Карта нормалей: «ёлочка» из наклонов влево-вправо. В файле НЕТ атрибута
    # TANGENT — и это часть проверки: касательные движок обязан посчитать сам,
    # иначе рельеф либо не появится, либо ляжет как попало.
    def normal_texel(x, y):
        tilt = 60 if (x // 2 + y // 2) % 2 else -60
        return (128 + tilt, 128, 235, 255)
    normal_png = png_rgba(8, 8, normal_texel)
    mr_view = add(mr_png)
    mask_view = add(mask_png)
    normal_view = add(normal_png)

    root = {
        'asset': {'version': '2.0', 'generator': 'SAGE test rig generator'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': nodes,
        'meshes': [
            {'name': 'Body', 'primitives': [{'attributes': body, 'indices': body_idx, 'material': 0}]},
            {'name': 'RigidPart', 'primitives': [{'attributes': rigid, 'indices': rigid_idx, 'material': 1}]},
            {'name': 'MaskPlate', 'primitives': [{'attributes': plate, 'indices': plate_idx, 'material': 2}]},
        ],
        'skins': [{'inverseBindMatrices': ibm_acc, 'joints': [1, 2], 'skeleton': 1}],
        'materials': [
            {'name': 'Body', 'pbrMetallicRoughness': {
                'baseColorFactor': [0.85, 0.82, 0.78, 1.0],
                'metallicFactor': 1.0, 'roughnessFactor': 1.0,
                'metallicRoughnessTexture': {'index': 0}},
             'occlusionTexture': {'index': 0},
             'normalTexture': {'index': 2}},
            {'name': 'Rigid', 'pbrMetallicRoughness': {
                'baseColorFactor': [0.12, 0.12, 0.14, 1.0],
                'metallicFactor': 0.0, 'roughnessFactor': 0.45},
             'emissiveFactor': [0.0, 0.9, 0.2]},
            {'name': 'Mask', 'doubleSided': True, 'alphaMode': 'MASK', 'alphaCutoff': 0.5,
             'pbrMetallicRoughness': {'baseColorTexture': {'index': 1},
                                      'metallicFactor': 0.0, 'roughnessFactor': 0.8}},
        ],
        'textures': [{'source': 0}, {'source': 1}, {'source': 2}],
        'images': [{'bufferView': mr_view, 'mimeType': 'image/png'},
                   {'bufferView': mask_view, 'mimeType': 'image/png'},
                   {'bufferView': normal_view, 'mimeType': 'image/png'}],
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

    print(f'{args.out}: частей 3 (скин + жёсткая на кости + пластинка с альфой), '
          f'костей 2, клип Bend, карты: нормали/металл-шероховатость/затенение/альфа, '
          f'{len(text) + len(blob)} байт')


if __name__ == '__main__':
    main()
