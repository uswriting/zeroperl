#!/usr/bin/env node
/**
 * delete.js
 *
 * This script reads a text file containing relative paths (one per line)
 * representing files or directories that exist within a given base directory.
 * It then deletes each file or directory
 *
 * Usage:
 *   ./delete.js <delete.txt> <base_directory>
 */

const fs = require('fs');
const path = require('path');

// Get the log file and base directory from the command-line arguments
const args = process.argv.slice(2);
if (args.length < 2) {
    console.error("Usage: delete.js <delete.txt> <base_directory>");
    process.exit(1);
}

const logFilePath = path.resolve(args[0]);
const baseDir = path.resolve(args[1]);

// Read the log file synchronously
let content;
try {
    content = fs.readFileSync(logFilePath, 'utf8');
} catch (err) {
    console.error(`Error reading file ${logFilePath}:`, err.message);
    process.exit(1);
}

// Split the content into lines, trim whitespace, and remove empty lines.
let entries = content
    .split(/\r?\n/)
    .map(line => line.trim())
    .filter(line => line.length > 0);

// Reverse the array so that nested files/directories are processed first.
entries.reverse();

// Process each entry synchronously.
for (const relativeEntry of entries) {
    // The target file/directory is located at baseDir/relativeEntry
    const targetPath = path.resolve(baseDir, relativeEntry);

    try {
        const stat = fs.lstatSync(targetPath);
        if (stat.isDirectory()) {
            // Remove the directory (non-recursively; directory must be empty)
            fs.rmdirSync(targetPath);
            console.log(`Deleted directory: ${relativeEntry}`);
        } else {
            // Remove a file or symbolic link
            fs.unlinkSync(targetPath);
            console.log(`Deleted file: ${relativeEntry}`);
        }
    } catch (err) {
        console.error(`Error deleting "${relativeEntry}":`, err.message);
    }
}
