// Provide MSVC-mangled operator delete that MinGW's .obj linker expects
#include <cstdlib>
void __cdecl operator delete(void* p) { free(p); }
void __cdecl operator delete[](void* p) { free(p); }
