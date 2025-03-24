# textc - offline text mesh compilation tool


### building msdfgen
```
git clone https://github.com/microsoft/vcpkg
git clone https://github.com/Chlumsky/msdfgen
cd vcpkg
./bootstrap-vcpkg.sh
export VCPKG_ROOT="$(pwd)"
cd ../msdfgen
mkdir build
cd build
cmake ..
cmake --build .
```

### *.textc binary format

```rust
struct str {
    u8             len;
    u8[len]        contents;
    u8[-(len+1)&3] alignment;
};

struct TextcFile {
    u8[3] magic = "TXT";
    u8  version;
    u32 vertex_size;
    u32 index_size;
    u32 num_strings;
    for num_strings {
        str name;
        u32 width;
        u32 height;
        u32 num_pages;
        for num_pages {
            u32 num_ranges;
            for num_ranges {
                str name;
                u32 start_idx;
                u32 end_ix;
            }
            u32 vertex_count;
            for vertex_count {
                f32 x, y, u, v;
            }
        }
    }
};
```


### generating an index buffer for a given vertex buffer

```c
size_t textc_index_buffer_size(uint32_t vertex_count) {
    return (vertex_count / 4) * 6;
}

void textc_fill_index_buffer(uint16_t* buffer, size_t count) {
    uint16_t v = 0;
    for (size_t i = 0; i < count; i += 4) {
        buffer[i] = v + 0;
        buffer[i + 1] = v + 1;
        buffer[i + 2] = v + 2;
        buffer[i + 3] = v + 2;
        buffer[i + 4] = v + 3;
        buffer[i + 5] = v + 0;
    }
}
```