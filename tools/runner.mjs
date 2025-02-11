#!/usr/bin/env node
import { readFile } from 'node:fs/promises';
import { WASI } from 'node:wasi';
import { instantiate } from './asyncify.mjs';

(async () => {

    const [, , wasmPath, ...args] = process.argv;

    if (!wasmPath) {
        console.error('Usage: runner <path-to-wasm> [arguments...]');
        process.exit(1);
    }

    // Create a new WASI instance
    const wasi = new WASI({
        version: 'preview1',
        args: ['zeroperl', '-V'], 
        env: {
            LC_ALL: 'C',
        },
        preopens: {
            '/': '/',
        },
        returnOnExit: false,
    });

    // Create the import object for the WASM module
    const imports = {
        ...wasi.getImportObject(),
    };

    // Load and instantiate the WASM
    const wasmBuffer = await readFile(wasmPath);
    const { instance } = await instantiate(wasmBuffer, imports);
    console.log('WASM loaded successfully');

    // Start the WASI application
    wasi.start(instance);
})();
