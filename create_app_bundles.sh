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

# Create app bundles for all GUI applications
create_app_bundle "File Tracker" "file_tracker_gui" "com.filetracker.gui"
create_app_bundle "File Tracker Logs" "ft_logs_gui" "com.filetracker.logs"
create_app_bundle "File Tracker Summary" "ft_summary_gui" "com.filetracker.summary"
create_app_bundle "File Tracker Drives" "ft_drives_gui" "com.filetracker.drives"

echo ""
echo "All .app bundles created successfully!"
echo "You can now drag these to /Applications or launch them from Finder."
echo "The apps will launch without showing a terminal window."
