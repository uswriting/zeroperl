#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');

// -----------------------------------------------------------------------------
// Simple command‐line argument parser.
const args = process.argv.slice(2);
let inputPath = '';
let outputPath = ''; // header file output (e.g. "sfs.h")
let prefix = '';
let skipRegex = '';

function printUsageAndExit() {
    console.error(`Usage: ${path.basename(process.argv[1])} --input-path <dir> --output-path <header file> [--prefix <prefix>] [--skip <regex>]\n`);
    process.exit(1);
}

for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === '--input-path' || arg === '-i') {
        inputPath = args[++i];
    } else if (arg.startsWith('--input-path=')) {
        inputPath = arg.split('=')[1];
    } else if (arg === '--output-path' || arg === '-o') {
        outputPath = args[++i];
    } else if (arg.startsWith('--output-path=')) {
        outputPath = arg.split('=')[1];
    } else if (arg === '--prefix') {
        prefix = args[++i];
    } else if (arg.startsWith('--prefix=')) {
        prefix = arg.split('=')[1];
    } else if (arg === '--skip') {
        skipRegex = args[++i];
    } else if (arg.startsWith('--skip=')) {
        skipRegex = arg.split('=')[1];
    } else {
        console.error(`Unknown argument: ${arg}`);
        printUsageAndExit();
    }
}

if (!inputPath) {
    console.error("Error: --input-path is required.");
    printUsageAndExit();
}
if (!outputPath) {
    console.error("Error: --output-path is required.");
    printUsageAndExit();
}

inputPath = path.resolve(inputPath);
if (!fs.existsSync(inputPath) || !fs.statSync(inputPath).isDirectory()) {
    console.error("Input path does not exist or is not a directory");
    process.exit(1);
}

// -----------------------------------------------------------------------------
// Traverse the input directory and collect file data.
let files = [];     // full paths of files
let relpaths = [];  // relative paths (UNIX-style) of files
let safepaths = []; // "safe" names (by replacing '/', '.', '-' with '_')
let fileDatas = []; // Buffer for each file’s content

function traverseDir(currentDir) {
    const entries = fs.readdirSync(currentDir);
    for (const entry of entries) {
        // (Do not skip dotfiles unless a skip regex is provided.)
        const fullPath = path.join(currentDir, entry);
        const stat = fs.statSync(fullPath);
        const rel = path.relative(inputPath, fullPath);
        if (skipRegex && new RegExp(skipRegex).test(rel)) {
            continue;
        }
        if (stat.isDirectory()) {
            traverseDir(fullPath);
        } else if (stat.isFile()) {
            files.push(fullPath);
            // Convert to UNIX-style (forward slashes) relative path.
            const relUnix = rel.split(path.sep).join('/');
            relpaths.push(relUnix);
            // Generate a safe name for use in symbol naming.
            const safe = relUnix.replace(/[\/\.\-]/g, '_');
            safepaths.push(safe);
            fileDatas.push(fs.readFileSync(fullPath));
        }
    }
}
traverseDir(inputPath);

// -----------------------------------------------------------------------------
// Compute offsets and sizes for each file’s data in a concatenated blob.
let offsets = [];
let sizes = [];
let totalSize = 0;
for (let i = 0; i < fileDatas.length; i++) {
    offsets.push(totalSize);
    let size = fileDatas[i].length;
    sizes.push(size);
    totalSize += size;
}

// Create one Buffer containing all file data concatenated.
let allData = Buffer.concat(fileDatas, totalSize);

// -----------------------------------------------------------------------------
// Generate the header file (e.g. sfs.h).
// This header defines a struct for each virtual file and exports the map.
let headerLines = [];
headerLines.push('#ifndef SFS_H');
headerLines.push('#define SFS_H');
headerLines.push('');
headerLines.push('#include <stddef.h>');
headerLines.push('');
headerLines.push('#ifdef __cplusplus');
headerLines.push('extern "C" {');
headerLines.push('#endif');
headerLines.push('');
headerLines.push(`#define SFS_BUILTIN_PREFIX "${prefix}"`);
headerLines.push('');
headerLines.push('struct sfs_entry {');
headerLines.push('    const char *abspath;    // Virtual absolute path (prefix + relative path)');
headerLines.push('    const char *safepath;   // A safe version of the relative path');
headerLines.push('    const unsigned char *start;  // Pointer into the data blob');
headerLines.push('    const unsigned char *end;    // Pointer just past the end of the file data');
headerLines.push('};');
headerLines.push('');
headerLines.push('extern size_t sfs_builtin_files_num;');
headerLines.push('extern const struct sfs_entry sfs_entries[];');
headerLines.push('');
headerLines.push('#ifdef __cplusplus');
headerLines.push('}');
headerLines.push('#endif');
headerLines.push('');
headerLines.push('#endif // SFS_H');

const headerContent = headerLines.join('\n');
fs.writeFileSync(outputPath, headerContent);
console.log(`Wrote header file: ${outputPath}`);

// -----------------------------------------------------------------------------
// Generate the data source file (e.g. sfs_data.c).
// We will generate one giant blob for all file data and an array of struct entries.
const dataOutputPath = outputPath.replace(/(\.h)?$/, '_data.c');
let dataLines = [];
dataLines.push(`#include "${path.basename(outputPath)}"`);
dataLines.push('');
// Define the number of files.
dataLines.push(`size_t sfs_builtin_files_num = ${fileDatas.length};`);
dataLines.push('');

// Generate the concatenated binary blob.
dataLines.push('const unsigned char sfs_builtin_data[] = {');
let hexArr = [];
for (let i = 0; i < allData.length; i++) {
    hexArr.push('0x' + allData[i].toString(16).padStart(2, '0'));
}
const bytesPerLine = 16;
for (let i = 0; i < hexArr.length; i += bytesPerLine) {
    dataLines.push('    ' + hexArr.slice(i, i + bytesPerLine).join(', ') + (i + bytesPerLine < hexArr.length ? ',' : ''));
}
dataLines.push('};');
dataLines.push('');

// Now generate the mapping array.
dataLines.push('const struct sfs_entry sfs_entries[] = {');
for (let i = 0; i < fileDatas.length; i++) {
    // Compute the virtual absolute path.
    const virtualPath = prefix ? path.posix.join(prefix, relpaths[i]) : relpaths[i];
    // Escape any double quotes.
    const abspathEscaped = virtualPath.replace(/"/g, '\\"');
    const safepathEscaped = safepaths[i].replace(/"/g, '\\"');
    dataLines.push(`    { "${abspathEscaped}", "${safepathEscaped}", sfs_builtin_data + ${offsets[i]}, sfs_builtin_data + ${offsets[i]} + ${sizes[i]} },`);
}
dataLines.push('};');

const dataContent = dataLines.join('\n');
fs.writeFileSync(dataOutputPath, dataContent);
console.log(`Wrote data source file: ${dataOutputPath}`);
