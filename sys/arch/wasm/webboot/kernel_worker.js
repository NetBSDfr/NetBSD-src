// NetBSD/wasm browser host - kernel worker.
//
// Stages the root image into linear memory, instantiates netbsd.wasm
// with the host ABI, and calls kernel_boot().  This never returns
// (the kernel scheduler is an infinite loop); a clean end only comes
// from the host.exit() import or a trap.
//
// Root image staging ABI: keep these three constants in sync with
// sys/arch/wasm/include/rootimg.h (single source of truth).
const ROOT_BASE = 0x0c000000; // WASM_ROOTIMG_ADDR
const ROOT_SIZE = 0x01300000; // WASM_ROOTIMG_SIZE
const MEM_PAGES = 4096;       // 256 MiB linear memory

// Host ABI (module "host"), implemented for the browser here:
//   console_write(i32)  one byte of kernel console output
//   now() -> i64        monotonic microseconds (BOOT_DURATION)
//   exit()              cpu_reboot(): stop the kernel
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
    },
  };

  report("instantiating...");
  const { instance } = await WebAssembly.instantiate(wasmBytes, importObject);

  report("running");
  try {
    instance.exports.kernel_boot();
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
