#!/bin/bash

# Clear the terminal display window frame
clear

echo "===================================================="
echo "⚡ FONTAINE OS UNIVERSAL DEPLOYMENT & GIT ENGINE ⚡"
echo "===================================================="

# 1. Sweep out older intermediate binary files
echo "🧹 Scrubbing old object assets..."
make clean

# 2. Trigger the primary project compilation pipeline
echo "🔨 Executing core compiler and linker stages..."
make bin/fontaineos.bin

# 3. Verify if the kernel compiled successfully
if [ $? -eq 0 ]; then
    echo "✅ Compilation successful! FontaineOS Image Secured."

    # Create a timestamped historical backup point inside bin/
    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    cp bin/fontaineos.bin bin/fontaineos_backup_${TIMESTAMP}.bin
    echo "📦 Saved historical kernel state backup: fontaineos_backup_${TIMESTAMP}.bin"

    # 4. Automate the remote repository deployment tracks
    echo "🌐 Syncing production code updates straight to GitHub..."
    git add .
    git commit -m "feat: master pipeline automation build cycle execution at ${TIMESTAMP}"
    git push origin main

    # 5. Boot up the hardware virtualization display console asynchronously
    echo "🚀 Booting FontaineOS inside QEMU Emulation Core..."
    make run &
else
    echo "❌ CRITICAL: Compilation failed. Aborting Git tracking and execution steps."
    exit 1
fi
