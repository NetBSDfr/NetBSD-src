


#ifndef _MACHINE_WASM_MODULE_H
#define _MACHINE_WASM_MODULE_H

#ifndef __WASM_EXPORT
#define __WASM_EXPORT(name) __attribute__((export_name(#name)))
#endif

#ifndef __WASM_IMPORT
#define __WASM_IMPORT(module, symbol) __attribute__((import_module(#module), import_name(#symbol)))
#endif

#endif /* _MACHINE_WASM_MODULE_H */