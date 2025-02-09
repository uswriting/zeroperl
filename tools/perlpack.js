#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const child_process = require("child_process");

// --- Parse command-line options ---
// We'll use a simple parser. Options:
//   --input-path (or -i)
//   --output-file (or -o)
//   --prefix
//   --ld (default: "ld")
//   --skip
const args = process.argv.slice(2);
let inputPath = "";
let outputFile = "";
let prefix = "";
let ld = "ld";
let skip = "";

for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === "--input-path" || arg === "-i") {
        inputPath = args[++i];
    } else if (arg === "--output-file" || arg === "-o") {
        outputFile = args[++i];
    } else if (arg === "--prefix") {
        prefix = args[++i];
    } else if (arg === "--ld") {
        ld = args[++i];
    } else if (arg === "--skip") {
        skip = args[++i];
    }
}

// Check required parameters
if (!inputPath || !fs.existsSync(inputPath) || !fs.statSync(inputPath).isDirectory()) {
    console.error("Input path does not exist or is not a directory");
    process.exit(1);
}
if (!outputFile) {
    console.error("Output file not specified");
    process.exit(1);
}
if (!prefix) {
    console.error("Prefix not specified");
    process.exit(1);
}

// --- Create directory for intermediate object files ---
const objDir = outputFile + ".o";
if (!fs.existsSync(objDir)) {
    fs.mkdirSync(objDir, { recursive: true });
}

// Arrays to collect information
const objects = [];
const filesArr = [];
const safePaths = [];
const relPaths = [];

// Recursive directory walk function
function walkDir(currentDir) {
    const items = fs.readdirSync(currentDir);
    for (const item of items) {
        const fullPath = path.join(currentDir, item);
        const stats = fs.statSync(fullPath);
        if (stats.isDirectory()) {
            walkDir(fullPath);
        } else if (stats.isFile()) {
            // If a skip regex is provided, skip matching files
            if (skip && new RegExp(skip).test(item)) {
                continue;
            }
            filesArr.push(fullPath);
            // Create a safe symbol name: replace '/', '.', and '-' with '_'
            const safe = fullPath.replace(/[\/\.\-]/g, "_");
            safePaths.push(safe);
            // Get the relative path from inputPath
            const relative = path.relative(inputPath, fullPath);
            relPaths.push(relative);
            // Determine object file name
            const objFile = path.join(objDir, safe + ".o");
            objects.push(objFile);
            // Run the linker command to generate the object file.
            // We call: ld -r -b binary -o <objFile> <fullPath>
            const ldResult = child_process.spawnSync(ld, ["-r", "-b", "binary", "-o", objFile, fullPath], { stdio: "inherit" });
            if (ldResult.status !== 0) {
                console.error(`ld command failed on ${fullPath}`);
                process.exit(1);
            }
        }
    }
}

// Walk the input directory
walkDir(inputPath);

// --- Generate the header file ---
let headerContent = "";
headerContent += "#ifndef PERLPACK_H\n#define PERLPACK_H\n\n";
headerContent += `#define PACKFS_BUILTIN_PREFIX "${prefix}"\n\n`;
headerContent += `size_t packfs_builtin_files_num = ${filesArr.length};\n\n`;

// Build absolute paths array: concatenate prefix with each relative path (using posix-style separators)
const absPaths = relPaths.map(rel => path.posix.join(prefix, rel));
headerContent += "const char* packfs_builtin_abspaths[] = {\n  \"" + absPaths.join('",\n  "') + "\"\n};\n\n";

// Print safe symbol names
headerContent += "const char* packfs_builtin_safepaths[] = {\n  \"" + safePaths.join('",\n  "') + "\"\n};\n\n";

// Declare extern symbols for each file.
safePaths.forEach(safe => {
    headerContent += `extern const char _binary_${safe}_start[];\n`;
    headerContent += `extern const char _binary_${safe}_end[];\n`;
});
headerContent += "\n";

// Define arrays of pointers to the file data.
headerContent += "const char* packfs_builtin_starts[] = {\n  " + safePaths.map(safe => `_binary_${safe}_start`).join(",\n  ") + "\n};\n\n";
headerContent += "const char* packfs_builtin_ends[] = {\n  " + safePaths.map(safe => `_binary_${safe}_end`).join(",\n  ") + "\n};\n\n";
headerContent += "#endif\n";

// Write the header file.
fs.writeFileSync(outputFile, headerContent);
console.log(`${outputFile} generated successfully.`);

// --- Write out the object file list --- 
const objectListFile = outputFile + ".txt";
fs.writeFileSync(objectListFile, objects.join("\n"));
console.log(`${objectListFile} generated successfully.`);
