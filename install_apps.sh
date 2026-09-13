#!/bin/bash

echo "Installing File Tracker GUI Applications to /Applications..."
echo ""

# Copy each .app bundle to /Applications
for app in "File Tracker.app" "File Tracker Logs.app" "File Tracker Summary.app" "File Tracker Drives.app" "File Tracker Unified.app"; do
    if [ -d "$app" ]; then
        echo "Installing $app..."
        cp -r "$app" /Applications/
    else
        echo "Warning: $app not found. Run 'make apps' first."
    fi
done

echo ""
echo "Installation complete!"
echo "The apps are now available in your Applications folder."
echo "You can also add them to your Dock by dragging them from /Applications."
