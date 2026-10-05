"""Convert an example WAT using the pinned Wasmtime C API (no global tool installation)."""
import ctypes
import sys
from pathlib import Path

class ByteVec(ctypes.Structure):
    _fields_ = [("size", ctypes.c_size_t), ("data", ctypes.c_void_p)]

def compile_wat(sdk_library, text):
    dll = ctypes.CDLL(str(sdk_library))
    dll.wasmtime_wat2wasm.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ByteVec)]
    dll.wasmtime_wat2wasm.restype = ctypes.c_void_p
    dll.wasm_byte_vec_delete.argtypes = [ctypes.POINTER(ByteVec)]
    dll.wasmtime_error_message.argtypes = [ctypes.c_void_p, ctypes.POINTER(ByteVec)]
    dll.wasmtime_error_delete.argtypes = [ctypes.c_void_p]
    raw = text.encode("utf-8")
    vec = ByteVec()
    error = dll.wasmtime_wat2wasm(raw, len(raw), ctypes.byref(vec))
    if error:
        message=ByteVec()
        dll.wasmtime_error_message(error,ctypes.byref(message))
        reason=ctypes.string_at(message.data,message.size).decode("utf-8")
        dll.wasm_byte_vec_delete(ctypes.byref(message))
        dll.wasmtime_error_delete(error)
        raise RuntimeError(reason)
    result=ctypes.string_at(vec.data,vec.size)
    dll.wasm_byte_vec_delete(ctypes.byref(vec))
    return result

if __name__=="__main__":
    library, source, output = map(Path,sys.argv[1:])
    output.write_bytes(compile_wat(library,source.read_text(encoding="utf-8")))
