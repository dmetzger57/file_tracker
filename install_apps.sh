#!/bin/bash

echo "Installing File Tracker Unified to /Applications..."
echo ""

# Copy the .app bundle to /Applications
if [ -d "File Tracker Unified.app" ]; then
    echo "Installing File Tracker Unified.app..."
    cp -r "File Tracker Unified.app" /Applications/
    echo ""
    echo "Installation complete!"
    echo "File Tracker Unified is now available in your Applications folder."
    echo "You can also add it to your Dock by dragging it from /Applications."
else
    echo "Error: File Tracker Unified.app not found."
    echo "Run 'make apps' first to create the app bundle."
    exit 1
fi
