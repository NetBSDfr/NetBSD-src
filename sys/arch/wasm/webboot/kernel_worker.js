// NetBSD/wasm browser host - kernel worker.
//
// Stages the root image into linear memory, instantiates netbsd.wasm
// with the host ABI, and calls kernel_boot().  This never returns
// (the kernel scheduler is an infinite loop); a clean end only comes
// from the host.exit() import or a trap.
//
// Root image staging ABI: keep these three constants in sync with
// sys/arch/wasm/include/rootimg.h (single source of truth).
const ROOT_BASE = 0x18000000; // WASM_ROOTIMG_ADDR
const ROOT_SIZE = 0x01300000; // WASM_ROOTIMG_SIZE
const MEM_PAGES = 8192;       // 512 MiB linear memory

// Host ABI (module "host"), implemented for the browser here:
//   console_write(i32)  one byte of kernel console output
//   now() -> i64        monotonic microseconds (BOOT_DURATION)
//   exit()              cpu_reboot(): stop the kernel
//   load_module(ptr, len) -> i32   compile+instantiate a user module
//                                  from linear memory (env.memory =
//                                  shared, env.svc -> kernel export)
//   call_start(handle, argp)       run a loaded module's _start
//   unload_module(handle)          drop the instance
//   wait(reason)                   blocking svc: suspend epoch (JSPI)
//   sleep(usec)                    clock_nanosleep: suspend epoch (JSPI)
class KernelExit extends Error {}

async function fetchBytes(url) {
  const r = await fetch(url);
  if (!r.ok)
    throw new Error(`GET ${url}: HTTP ${r.status} (see index.html usage)`);
  return r.arrayBuffer();
}

let outBuf = [];

function report(text, fatal = false) {
  postMessage({ done: text, fatal });
}

function flush() {
  if (outBuf.length) {
    postMessage({ out: new Uint8Array(outBuf) });
    outBuf = [];
  }
}

async function main() {
  const [wasmBytes, rootImg] = await Promise.all([
    fetchBytes("netbsd.wasm"),
    fetchBytes("rootimg.bin"),
  ]);
  if (rootImg.byteLength !== ROOT_SIZE) {
    throw new Error(`rootimg.bin is ${rootImg.byteLength} bytes, kernel expects ${ROOT_SIZE}`);
  }

  // --import-memory: the host owns linear memory so it can stage the
  // root image into it before any kernel code runs.
  const memory = new WebAssembly.Memory({ initial: MEM_PAGES });
  new Uint8Array(memory.buffer, ROOT_BASE, ROOT_SIZE).set(new Uint8Array(rootImg));
  outBuf.push(...new TextEncoder().encode(
      `[host] staged ${rootImg.byteLength} byte root image at 0x${ROOT_BASE.toString(16)}\n`));

  const t0 = performance.now();
  const userMods = { map: new Map() };
  const JSPI = typeof WebAssembly.Suspending === 'function'
    && typeof WebAssembly.promising === 'function';
  const waiters = [];
  // Suspending import for blocking svc: a returned Promise suspends
  // the wasm epoch (all frames between here and the promising
  // kernel_boot entry are wasm -- JS frames in between would trap);
  // a non-Promise return means input is already queued.
  const WAIT_INPUT = 0, WAIT_CHILD = 1;
  let runChild = null;
  let childBusy = false;
  const waitImpl = (reason) => {
    // Only an input wait short-circuits on queued console bytes.
    if (reason === WAIT_INPUT && instance.exports.wasm_console_has_input())
      return undefined;
    // Host-driven child scheduler, serial (see wasm_loader.c): start
    // a pending child only while no other child slice is suspended.
    if (!childBusy && runChild) {
      const idx = instance.exports.wasm_take_pending();
      if (idx >= 0) {
        childBusy = true;
        return runChild(idx).then(() => { childBusy = false; });
      }
    }
    return new Promise((res) => waiters.push(res));
  };
  const importObject = {
    env: { memory },
    host: {
      // Unbuffered: the kernel's prompt lines end without a newline, so
      // any buffering (even line buffering) would hide the init path
      // prompt until the next write.  Console volume is only a few KB.
      console_write: (c) => {
        outBuf.push(c & 0xff);
        flush();
      },
      now: () => BigInt(Math.round((performance.now() - t0) * 1000)),
      exit: () => { throw new KernelExit(); },
      // loader service: instantiate with env.svc wired straight to
      // the kernel export (no JS trampoline) and _start placed in the
      // kernel's function table; return the table slot.
      load_module: (ptr, len) => {
        try {
          const mbytes = new Uint8Array(memory.buffer, ptr >>> 0, len >>> 0).slice();
          const m = new WebAssembly.Module(mbytes);
          const mi = new WebAssembly.Instance(m, { env: {
            memory,
            svc: instance.exports.wasm_user_svc,
          }});
          if (typeof mi.exports._start !== 'function') return -1;
          const table = instance.exports.__indirect_function_table;
          const slot = table.length;
          table.grow(1);
          table.set(slot, mi.exports._start);
          userMods.map.set(slot, mi);
          return slot;
        } catch (e) {
          report(`host: load_module: ${e.message}`, true);
          return -1;
        }
      },
      unload_module: (h) => {
        try { instance.exports.__indirect_function_table.set(h >>> 0, null); } catch (e) {}
        userMods.map.delete(h >>> 0);
      },
      wait: JSPI ? new WebAssembly.Suspending(waitImpl) : () => {},
      // Host-driven timed sleep: the kernel has no timer interrupt, so
      // clock_nanosleep suspends the epoch here for usec microseconds.
      sleep: JSPI ? new WebAssembly.Suspending(
        (usec) => new Promise((res) => setTimeout(res, usec / 1000))) : () => {},
    },
  };

  report("instantiating...");
  const { instance } = await WebAssembly.instantiate(wasmBytes, importObject);
  if (JSPI)
    runChild = WebAssembly.promising(instance.exports.wasm_run_child);

  instance.exports.wasm_set_jspi(JSPI ? 1 : 0);

  // console input from the UI thread: { in: Uint8Array }
  onmessage = ({ data }) => {
    if (!data || data.in === undefined) return;
    for (const b of new Uint8Array(data.in))
      instance.exports.wasm_console_input(b);
    const ws = waiters.splice(0);
    for (const r of ws) r();
  };

  report("running");
  const boot = JSPI
    ? WebAssembly.promising(instance.exports.kernel_boot)
    : instance.exports.kernel_boot;
  try {
    await boot();
    flush();
    report("kernel_boot() returned (unexpected)");
  } catch (e) {
    flush();
    if (e instanceof KernelExit)
      report("kernel exited");
    else
      report(`TRAP: ${e.message}`, true);
  }
}

main().catch((e) => report(`host: ${e.message}`, true));
