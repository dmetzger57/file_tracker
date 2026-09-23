#!/bin/bash

# Function to create a .app bundle
create_app_bundle() {
    local APP_NAME=$1
    local EXECUTABLE=$2
    local BUNDLE_ID=$3

    echo "Creating ${APP_NAME}.app..."

    # Start from an empty bundle so files from earlier builds don't linger
    rm -rf "${APP_NAME}.app"

    # Create bundle structure
    mkdir -p "${APP_NAME}.app/Contents/MacOS"
    mkdir -p "${APP_NAME}.app/Contents/Resources"

    # Use the real binary as the bundle executable (no wrapper process), so the
    # Dock shows a single icon tied to this bundle
    cp "$EXECUTABLE" "${APP_NAME}.app/Contents/MacOS/${EXECUTABLE}"

    # Copy app icon
    cp icon/AppIcon.icns "${APP_NAME}.app/Contents/Resources/AppIcon.icns"

    # Create Info.plist
    cat > "${APP_NAME}.app/Contents/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>${EXECUTABLE}</string>
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
