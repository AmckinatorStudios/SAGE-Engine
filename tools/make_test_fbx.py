#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Собирает маленький ДВОИЧНЫЙ .fbx для тестов импорта.

ЗАЧЕМ СВОЙ ГЕНЕРАТОР. Проверять импорт FBX не на чем: класть в репозиторий
чужой файл из редактора — значит тащить мегабайты и лицензию на модель, а
скачивать его в тестах нельзя (CI без сети). Файл, собранный здесь, содержит
ровно то, что разбирает импортёр: заголовок, GlobalSettings с единицами и осью,
Objects с Geometry (позиции, индексы полигонов, нормали, UV). Значения простые
и известны заранее — поэтому тест может сверять координаты, а не «загрузилось».

    python3 tools/make_test_fbx.py out.fbx [--zup] [--unit 100] [--ascii]
"""
import argparse
import struct
import sys

VERSION = 7400  # < 7500 — 32-битные смещения


def prop_string(text):
    data = text.encode('utf-8')
    return b'S' + struct.pack('<I', len(data)) + data


def prop_int(value):
    return b'I' + struct.pack('<i', value)


def prop_double(value):
    return b'D' + struct.pack('<d', value)


def prop_long(value):
    return b'L' + struct.pack('<q', value)


def prop_double_array(values):
    payload = struct.pack('<%dd' % len(values), *values)
    # encoding 0 — без сжатия: импортёр умеет и сжатые, но для теста важнее,
    # чтобы файл читался глазами в hex при разборе полётов.
    return b'd' + struct.pack('<III', len(values), 0, len(payload)) + payload


def prop_int_array(values):
    payload = struct.pack('<%di' % len(values), *values)
    return b'i' + struct.pack('<III', len(values), 0, len(payload)) + payload


def node(name, props=b'', children=()):
    """Одна запись FBX. Возвращает функцию, которой нужно смещение начала."""
    def build(offset):
        name_bytes = name.encode('utf-8')
        header_len = 4 + 4 + 4 + 1 + len(name_bytes)
        child_blobs = []
        cursor = offset + header_len + len(props)
        for child in children:
            blob = child(cursor)
            child_blobs.append(blob)
            cursor += len(blob)
        if children:
            cursor += 13  # нулевая запись — конец списка детей
        end_offset = cursor
        out = struct.pack('<III', end_offset, count_props(props), len(props))
        out += struct.pack('<B', len(name_bytes)) + name_bytes + props
        out += b''.join(child_blobs)
        if children:
            out += b'\0' * 13
        return out
    return build


def count_props(props):
    """Сколько свойств в уже собранном блоке (типы известны — считаем по ним)."""
    n, i = 0, 0
    while i < len(props):
        t = props[i:i + 1]
        n += 1
        i += 1
        if t == b'I': i += 4
        elif t == b'D': i += 8
        elif t == b'L': i += 8
        elif t in (b'S', b'R'):
            size = struct.unpack('<I', props[i:i + 4])[0]
            i += 4 + size
        elif t in (b'd', b'f', b'i', b'l', b'b'):
            count, _enc, comp = struct.unpack('<III', props[i:i + 12])
            i += 12 + comp
        else:
            raise ValueError('неизвестный тип свойства: %r' % t)
    return n


def p70v(name, values):
    """Свойство-вектор (Lcl Translation и подобные): три числа подряд."""
    props = prop_string(name) + prop_string('Lcl Translation') + prop_string('') + prop_string('A')
    for v in values:
        props += prop_double(float(v))
    return node('P', props)


def p70(name, kind, value):
    props = prop_string(name) + prop_string(kind) + prop_string('') + prop_string('')
    props += prop_int(value) if isinstance(value, int) else prop_double(value)
    return node('P', props)


# Куб со стороной 1 в СВОИХ единицах, центр в начале координат.
CUBE = [
    (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5),
    (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5),
]
QUADS = [
    (0, 1, 2, 3), (5, 4, 7, 6), (4, 0, 3, 7),
    (1, 5, 6, 2), (4, 5, 1, 0), (3, 2, 6, 7),
]


def build(path, zup=False, unit=100.0, ascii_mode=False, offset=(0.0, 0.0, 0.0), scale=1.0):
    if ascii_mode:
        with open(path, 'w', encoding='utf-8') as f:
            f.write('; FBX 6.1.0 project file\n; текстовый вариант — импортёр обязан сказать об этом прямо\n')
        return

    verts = []
    for x, y, z in CUBE:
        verts.extend([x, y, z] if not zup else [x, -z, y])

    indices = []
    for quad in QUADS:
        indices.extend([quad[0], quad[1], quad[2], ~quad[3]])  # последний — с ~

    # Нормали по вершинам полигонов (ByPolygonVertex): для теста достаточно
    # направления грани, посчитанного грубо по первой тройке.
    normals = []
    for quad in QUADS:
        a = CUBE[quad[0]]
        b = CUBE[quad[1]]
        c = CUBE[quad[2]]
        ux, uy, uz = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        vx, vy, vz = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        nx, ny, nz = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
        length = max((nx * nx + ny * ny + nz * nz) ** 0.5, 1e-9)
        n = (nx / length, ny / length, nz / length)
        if zup:
            n = (n[0], -n[2], n[1])
        for _ in range(4):
            normals.extend(n)

    uvs = [0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0] * len(QUADS)

    settings = node('GlobalSettings', b'', [
        node('Properties70', b'', [
            p70('UpAxis', 'int', 2 if zup else 1),
            p70('UnitScaleFactor', 'double', float(unit)),
        ]),
    ])

    geometry = node(
        'Geometry',
        prop_long(1234) + prop_string('Cube\x00\x01Geometry') + prop_string('Mesh'),
        [
            node('Vertices', prop_double_array(verts)),
            node('PolygonVertexIndex', prop_int_array(indices)),
            node('LayerElementNormal', prop_int(0), [
                node('MappingInformationType', prop_string('ByPolygonVertex')),
                node('ReferenceInformationType', prop_string('Direct')),
                node('Normals', prop_double_array(normals)),
            ]),
            node('LayerElementUV', prop_int(0), [
                node('MappingInformationType', prop_string('ByPolygonVertex')),
                node('ReferenceInformationType', prop_string('Direct')),
                node('UV', prop_double_array(uvs)),
            ]),
        ])

    # Узел Model с трансформом и СВЯЗЬЮ с геометрией: без связи импортёр не
    # знает, чьим узлом является меш, и трансформ применить не к чему.
    model_props = prop_long(4321) + prop_string('TestCube\x00\x01Model') + prop_string('Mesh')
    model = node('Model', model_props, [
        node('Properties70', b'', [
            p70v('Lcl Translation', offset),
            p70v('Lcl Scaling', (scale, scale, scale)),
        ]),
    ])
    objects = node('Objects', b'', [geometry, model])
    # Connections: Geometry (1234) -> Model (4321)
    connections = node('Connections', b'', [
        node('C', prop_string('OO') + prop_long(1234) + prop_long(4321)),
    ])

    header = b'Kaydara FBX Binary  \x00\x1a\x00' + struct.pack('<I', VERSION)
    body = b''
    cursor = len(header)
    for top in (settings, objects, connections):
        blob = top(cursor)
        body += blob
        cursor += len(blob)
    body += b'\0' * 13  # конец списка верхнего уровня

    with open(path, 'wb') as f:
        f.write(header + body)


def prop_long_array(values):
    payload = struct.pack('<%dq' % len(values), *values)
    return b'l' + struct.pack('<III', len(values), 0, len(payload)) + payload


def prop_float_array(values):
    payload = struct.pack('<%df' % len(values), *values)
    return b'f' + struct.pack('<III', len(values), 0, len(payload)) + payload


def p70_vec(name, kind, values):
    """Свойство-вектор с ЯВНЫМ типом: у PreRotation он свой, и подставлять
    везде «Lcl Translation», как делает p70v, нельзя."""
    props = prop_string(name) + prop_string(kind) + prop_string('') + prop_string('A')
    for v in values:
        props += prop_double(float(v))
    return node('P', props)


# Время FBX: 46186158000 единиц в секунде (делится на все частоты кадров).
KTIME = 46186158000


def build_skinned(path, unit=100.0):
    """Модель СО СКИНОМ: полоса из четырёх вершин на двух костях + клип.

    Геометрия нарочно простейшая, а веса — заведомо разные: нижние вершины
    держит только первая кость, верхние — только вторая. Поэтому поворот второй
    кости обязан двигать ровно верхние вершины, и проверке есть что измерить.
    """
    verts = [
        -1.0, 0.0, 0.0,   # 0 — низ слева
         1.0, 0.0, 0.0,   # 1 — низ справа
         1.0, 2.0, 0.0,   # 2 — верх справа
        -1.0, 2.0, 0.0,   # 3 — верх слева
    ]
    indices = [0, 1, 2, ~3]
    normals = [0.0, 0.0, 1.0] * 4
    uvs = [0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0]

    geometry = node(
        'Geometry', prop_long(100) + prop_string('Strip\x00\x01Geometry') + prop_string('Mesh'),
        [
            node('Vertices', prop_double_array(verts)),
            node('PolygonVertexIndex', prop_int_array(indices)),
            node('LayerElementNormal', prop_int(0), [
                node('MappingInformationType', prop_string('ByPolygonVertex')),
                node('ReferenceInformationType', prop_string('Direct')),
                node('Normals', prop_double_array(normals)),
            ]),
            node('LayerElementUV', prop_int(0), [
                node('MappingInformationType', prop_string('ByPolygonVertex')),
                node('ReferenceInformationType', prop_string('Direct')),
                node('UV', prop_double_array(uvs)),
            ]),
        ])

    mesh_model = node('Model', prop_long(200) + prop_string('SkinMesh\x00\x01Model') + prop_string('Mesh'),
                      [node('Properties70', b'', [])])

    # Кости: корень в начале координат, вторая — на высоте 1 (в единицах файла).
    # У второй ЕСТЬ PreRotation: именно его теряют наивные импортёры, и модель
    # выходит вывернутой, хотя «все числа прочитаны».
    root_bone = node('Model', prop_long(300) + prop_string('Root\x00\x01Model') + prop_string('LimbNode'),
                     [node('Properties70', b'', [p70v('Lcl Translation', (0.0, 0.0, 0.0))])])
    child_bone = node('Model', prop_long(301) + prop_string('Upper\x00\x01Model') + prop_string('LimbNode'),
                      [node('Properties70', b'', [
                          p70v('Lcl Translation', (0.0, 1.0, 0.0)),
                          p70_vec('PreRotation', 'Vector3D', (0.0, 0.0, 0.0)),
                      ])])

    def matrix_prop(m):
        return prop_double_array(m)

    ident = [1.0, 0.0, 0.0, 0.0,  0.0, 1.0, 0.0, 0.0,  0.0, 0.0, 1.0, 0.0,  0.0, 0.0, 0.0, 1.0]
    # TransformLink второй кости — её мировая матрица привязки (сдвиг на 1 по Y).
    link_child = [1.0, 0.0, 0.0, 0.0,  0.0, 1.0, 0.0, 0.0,  0.0, 0.0, 1.0, 0.0,  0.0, 1.0, 0.0, 1.0]

    skin = node('Deformer', prop_long(400) + prop_string('Skin\x00\x01Deformer') + prop_string('Skin'))
    cluster_root = node(
        'Deformer', prop_long(401) + prop_string('ClusterRoot\x00\x01SubDeformer') + prop_string('Cluster'),
        [
            node('Indexes', prop_int_array([0, 1])),
            node('Weights', prop_double_array([1.0, 1.0])),
            node('Transform', matrix_prop(ident)),
            node('TransformLink', matrix_prop(ident)),
        ])
    cluster_child = node(
        'Deformer', prop_long(402) + prop_string('ClusterUpper\x00\x01SubDeformer') + prop_string('Cluster'),
        [
            node('Indexes', prop_int_array([2, 3])),
            node('Weights', prop_double_array([1.0, 1.0])),
            node('Transform', matrix_prop(ident)),
            node('TransformLink', matrix_prop(link_child)),
        ])

    # Клип: вторая кость поворачивается вокруг Z с 0 до 90 градусов за секунду.
    stack = node('AnimationStack', prop_long(500) + prop_string('Wave\x00\x01AnimStack') + prop_string(''))
    layer = node('AnimationLayer', prop_long(501) + prop_string('Base\x00\x01AnimLayer') + prop_string(''))
    curve_node = node('AnimationCurveNode', prop_long(502) + prop_string('R\x00\x01AnimCurveNode') + prop_string(''),
                      [node('Properties70', b'', [])])
    curve_z = node('AnimationCurve', prop_long(503) + prop_string('\x00\x01AnimCurve') + prop_string(''), [
        node('KeyTime', prop_long_array([0, KTIME])),
        node('KeyValueFloat', prop_float_array([0.0, 90.0])),
    ])

    settings = node('GlobalSettings', b'', [
        node('Properties70', b'', [
            p70('UpAxis', 'int', 1),
            p70('UnitScaleFactor', 'double', float(unit)),
        ]),
    ])

    objects = node('Objects', b'', [geometry, mesh_model, root_bone, child_bone,
                                    skin, cluster_root, cluster_child,
                                    stack, layer, curve_node, curve_z])

    connections = node('Connections', b'', [
        node('C', prop_string('OO') + prop_long(100) + prop_long(200)),   # Geometry -> Model
        node('C', prop_string('OO') + prop_long(301) + prop_long(300)),   # Upper -> Root
        node('C', prop_string('OO') + prop_long(400) + prop_long(100)),   # Skin -> Geometry
        node('C', prop_string('OO') + prop_long(401) + prop_long(400)),   # Cluster -> Skin
        node('C', prop_string('OO') + prop_long(402) + prop_long(400)),
        node('C', prop_string('OO') + prop_long(300) + prop_long(401)),   # Root -> ClusterRoot
        node('C', prop_string('OO') + prop_long(301) + prop_long(402)),   # Upper -> ClusterUpper
        node('C', prop_string('OO') + prop_long(501) + prop_long(500)),   # Layer -> Stack
        node('C', prop_string('OO') + prop_long(502) + prop_long(501)),   # CurveNode -> Layer
        node('C', prop_string('OP') + prop_long(502) + prop_long(301) + prop_string('Lcl Rotation')),
        node('C', prop_string('OP') + prop_long(503) + prop_long(502) + prop_string('d|Z')),
    ])

    header = b'Kaydara FBX Binary  \x00\x1a\x00' + struct.pack('<I', VERSION)
    body = b''
    cursor = len(header)
    for top in (settings, objects, connections):
        blob = top(cursor)
        body += blob
        cursor += len(blob)
    body += b'\0' * 13

    with open(path, 'wb') as f:
        f.write(header + body)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--zup', action='store_true', help='ось Z вверх (как в 3ds Max)')
    ap.add_argument('--unit', type=float, default=100.0, help='UnitScaleFactor (см на единицу)')
    ap.add_argument('--ascii', action='store_true', help='текстовый FBX (для проверки отказа)')
    ap.add_argument('--offset', nargs=3, type=float, default=[0.0, 0.0, 0.0],
                    help='Lcl Translation узла Model')
    ap.add_argument('--node-scale', type=float, default=1.0, help='Lcl Scaling узла Model')
    ap.add_argument('--skin', action='store_true', help='модель со скином, костями и клипом')
    args = ap.parse_args()
    if args.skin:
        build_skinned(args.out, args.unit)
    else:
        build(args.out, args.zup, args.unit, args.ascii, tuple(args.offset), args.node_scale)
    print('записан', args.out)
