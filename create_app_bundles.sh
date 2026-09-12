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

    # Create wrapper script that launches without terminal
    cat > "${APP_NAME}.app/Contents/MacOS/launcher" << 'LAUNCHER'
#!/bin/bash
# Get the directory containing this script
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# Launch the actual executable from Resources, redirect output to /dev/null
exec "$DIR/../Resources/EXECUTABLE_PLACEHOLDER" > /dev/null 2>&1
LAUNCHER

    # Replace placeholder with actual executable name
    sed -i '' "s/EXECUTABLE_PLACEHOLDER/$EXECUTABLE/g" "${APP_NAME}.app/Contents/MacOS/launcher"

    # Make launcher executable
    chmod +x "${APP_NAME}.app/Contents/MacOS/launcher"

    # Create Info.plist - point to the wrapper launcher
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
    <key>LSUIElement</key>
    <false/>
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
