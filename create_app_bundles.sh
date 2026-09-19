#!/bin/bash

# Function to create a .app bundle
create_app_bundle() {
    local APP_NAME=$1
    local EXECUTABLE=$2
    local BUNDLE_ID=$3

    echo "Creating ${APP_NAME}.app..."

    # Create bundle structure
    mkdir -p "${APP_NAME}.app/Contents/MacOS"
    mkdir -p "${APP_NAME}.app/Contents/Resources"

    # Copy executable to Resources
    cp "$EXECUTABLE" "${APP_NAME}.app/Contents/Resources/"

    # Copy app icon
    cp icon/AppIcon.icns "${APP_NAME}.app/Contents/Resources/AppIcon.icns"

    # Create a customized launcher.m for this app
    sed "s/APP_EXECUTABLE/$EXECUTABLE/g" launcher.m > "${APP_NAME}.app/Contents/MacOS/launcher_temp.m"

    # Compile the Objective-C wrapper with Cocoa framework
    clang -framework Cocoa -o "${APP_NAME}.app/Contents/MacOS/launcher" "${APP_NAME}.app/Contents/MacOS/launcher_temp.m"

    # Remove temporary source file
    rm "${APP_NAME}.app/Contents/MacOS/launcher_temp.m"

    # Create Info.plist - point to the compiled launcher
    cat > "${APP_NAME}.app/Contents/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>launcher</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>CFBundleIdentifier</key>
    <string>$BUNDLE_ID</string>
    <key>CFBundleName</key>
    <string>$APP_NAME</string>
    <key>CFBundleDisplayName</key>
    <string>$APP_NAME</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleSignature</key>
    <string>????</string>
    <key>LSMinimumSystemVersion</key>
    <string>10.15</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST

    echo "Created ${APP_NAME}.app"
}

# Create app bundle for unified application
create_app_bundle "File Tracker Unified" "file_tracker_unified" "com.filetracker.unified"

echo ""
echo "App bundle created successfully!"
echo "You can now drag File Tracker Unified.app to /Applications or launch it from Finder."
echo "The app will launch without showing a terminal window."
