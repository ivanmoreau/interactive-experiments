"""Reads MATLAB level-5 .mat files (the format scipy.io.loadmat reads) with
only the standard library: numeric matrices, character arrays, cell
arrays, and structs, compressed or not. Enough for the connectome files."""

import struct
import zlib

MI_INT8, MI_UINT8, MI_INT16, MI_UINT16, MI_INT32, MI_UINT32 = 1, 2, 3, 4, 5, 6
MI_SINGLE, MI_DOUBLE, MI_INT64, MI_UINT64 = 7, 9, 12, 13
MI_MATRIX, MI_COMPRESSED, MI_UTF8, MI_UTF16, MI_UTF32 = 14, 15, 16, 17, 18
MX_CELL, MX_STRUCT, MX_OBJECT, MX_CHAR, MX_SPARSE = 1, 2, 3, 4, 5

NUMBER_FORMATS = {
    MI_INT8: "b", MI_UINT8: "B", MI_INT16: "h", MI_UINT16: "H", MI_INT32: "i", MI_UINT32: "I",
    MI_SINGLE: "f", MI_DOUBLE: "d", MI_INT64: "q", MI_UINT64: "Q",
}


class Matrix:
    """A numeric or character array: dims, and values in column-major order."""

    def __init__(self, dims, values):
        self.dims = dims
        self.values = values

    def __getitem__(self, index):
        """Element (row, column) of a 2-D matrix."""
        row, column = index
        return self.values[row + column * self.dims[0]]

    def __len__(self):
        return len(self.values)


class Reader:
    def __init__(self, data, endian):
        self.data = data
        self.endian = endian
        self.offset = 0

    def element(self):
        """Returns (type, payload bytes) and moves past padding."""
        (tag,) = struct.unpack_from(self.endian + "I", self.data, self.offset)
        if tag >> 16:  # small element: type and size share the tag, data in 4 bytes
            kind, size = tag & 0xFFFF, tag >> 16
            payload = self.data[self.offset + 4:self.offset + 4 + size]
            self.offset += 8
            return kind, payload
        kind, size = struct.unpack_from(self.endian + "II", self.data, self.offset)
        start = self.offset + 8
        payload = self.data[start:start + size]
        self.offset = start + size
        if kind != MI_COMPRESSED:
            self.offset += (-size) % 8
        return kind, payload

    def numbers(self, kind, payload):
        if kind in (MI_UTF8,):
            return list(payload.decode("utf-8"))
        code = NUMBER_FORMATS[kind]
        count = len(payload) // struct.calcsize(code)
        return list(struct.unpack(self.endian + code * count, payload[:count * struct.calcsize(code)]))


def parse_matrix(payload, endian):
    """Parses the contents of a miMATRIX element."""
    reader = Reader(payload, endian)
    if not payload:
        return None
    _, flags = reader.element()
    flag_word = struct.unpack(endian + "I", flags[:4])[0]
    array_class = flag_word & 0xFF
    is_complex = bool(flag_word & 0x800)
    _, dims_bytes = reader.element()
    dims = list(struct.unpack(endian + "i" * (len(dims_bytes) // 4), dims_bytes))
    _, name = reader.element()
    count = 1
    for d in dims:
        count *= d
    if array_class == MX_CELL:
        cells = []
        for _ in range(count):
            kind, inner = reader.element()
            cells.append(parse_matrix(inner, endian))
        return Matrix(dims, cells)
    if array_class == MX_STRUCT:
        _, length_bytes = reader.element()
        field_length = struct.unpack(endian + "i", length_bytes[:4])[0]
        _, names_bytes = reader.element()
        fields = [names_bytes[i:i + field_length].split(b"\0", 1)[0].decode() for i in range(0, len(names_bytes), field_length)]
        records = []
        for _ in range(count):
            record = {}
            for field in fields:
                kind, inner = reader.element()
                record[field] = parse_matrix(inner, endian)
            records.append(record)
        return records[0] if count == 1 else records
    if array_class in (MX_OBJECT, MX_SPARSE):
        raise ValueError("object and sparse arrays are not supported")
    kind, real = reader.element()
    values = reader.numbers(kind, real)
    if is_complex:
        raise ValueError("complex arrays are not supported")
    if array_class == MX_CHAR:
        text = "".join(chr(v) if isinstance(v, int) else v for v in values)
        rows = dims[0] if dims else 0
        if rows <= 1:
            return text
        columns = len(text) // rows
        return ["".join(text[r + c * rows] for c in range(columns)).rstrip() for r in range(rows)]
    return Matrix(dims, values)


def load(path):
    """Returns {variable name: value} for a .mat file."""
    with open(path, "rb") as file:
        data = file.read()
    if len(data) < 128:
        raise ValueError("not a MATLAB 5 file")
    endian = "<" if data[126:128] == b"IM" else ">"
    result = {}
    reader = Reader(data, endian)
    reader.offset = 128
    while reader.offset + 8 <= len(data):
        kind, payload = reader.element()
        if kind == MI_COMPRESSED:
            payload = zlib.decompress(payload)
            inner = Reader(payload, endian)
            kind, payload = inner.element()
        if kind != MI_MATRIX:
            continue
        name_reader = Reader(payload, endian)
        name_reader.element()
        name_reader.element()
        _, name = name_reader.element()
        result[name.decode("ascii", "replace")] = parse_matrix(payload, endian)
    return result
