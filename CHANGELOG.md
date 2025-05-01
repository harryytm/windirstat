# WinDirStat 2.2.2

## Enhancements
- Traditional Chinese language support (thanks @harryytm)
- Korean languages updates (thanks @VenusGirl)
- Gray-out user defined submenu if none are present
- Basic support for scanning \\\\?\\Volume{GUID} formatted paths
- Fallback deletion for hiberfil.sys 
- Various performance enhancements

## Bug Fixes
- Corrected hash entry inconsistencies in duplicate viewer
- Corrected tallying of physical sizes in duplicate viewer
- Corrected treemap zoomed view hit detection and highlighting
- Corrected treemap custom grid color not being applied

# WinDirStat 2.2.1

## Enhancements
* Added "Largest Files" tab to the interface
* Increased the number of colorized extensions in TreeMap
* Added file extension information to duplicate list
* Numerous duplicate detection performance improvements
* Performance improvements when refreshing selected items
* Added Korean language support (thanks @VenusGirl)
* Added cleanup option to disable hibernate (hiberfil.sys)
* Added additional metadata to MSI installer
* Modified chocolatey installer to allow internalization
* Allow production / beta to update each other
* Improved legacy installer cleanup logic

## Bug Fixes
* Fixed missing overlays on some icons (e.g. .lnk files)
* Right-aligned numerical data in columns
* Allow WinDirStat.exe to accept folder as command line argument
* Addressed numerous potential hanging / crashing scenarios
* Addressed copy / paste not always working
* Addressed hiding toolbar and status bar setting persistence
  
# WinDirStat 2.1.1

## Enhancements
* Ability to exclude folders by path
* Ability to exclude files by name
* Ability to exclude files by minimum file size
* Scans now stop quicker when requested during duplicate scan
* Slightly reduced executable size
* Better Norwegian tranlsations (thanks @TilKenneth)
* Improved keyboard navigation on the file deletion dialog box
* Cleanup option to empty folder
* Improved file deletion progress indicator
* Display free space percentage next to volume label
* Other translations improvements (thanks @EricPossato, @tferrerm)
  
## Bug Fixes
* Addressed not being able to scan CSC directory
* Addressed not being to scan SUBST'd drives
* Addressed save/load files on Windows Server 2016 not working
* Addressed hover over treemap not showing filename properly
* Addressed not being able to scan in some Acronis folders

# WinDirStat 2.0.3

## Enhancements
* Added status pane space usage summation for selected items
* Added attribute display character for sparse file (Z)

## Bug Fixes
* Addressed MSI installer not cleaning up old version properly
* Addressed potential hang when rendering tree icons
* Addressed behavior when calculating size for docker images
* Addressed size format not displaying after setting change 
* Addressed Norwegian language loading Dutch language
* Addressed Portuguese mistranslation (thanks @PedroBittarBarao)
* Addressed various typos in code comments (thanks @NathanBaulch)
  
# WinDirStat 2.0.1

## Enhancements
* Multiple item selection
* Scanning performance enhancements
* Duplicate file finder based on file hashes
* Native 64-bit build now available
* Native ARM build now available
* Switched to MSI-based installer
* Menu shortcuts for popular native cleanup utilities
* Portable settings mode using WinDirStat.ini file
* Export scan results to CSV file
* Compress files using transparent compression capabilities
* Context menu option to display full Explorer context menu
* Context menu option to launch PowerShell
* Right-click explorer menu
* Toolbar icons enhanced
* Long file names are now supported
* Option to relaunch with elevated credentials
* Utilize backup / restore privileges to scan inaccessible files
* Pacman drawing enhancements
* Resolution scaling improvements
* Reparse point scanning exclusions
* Per-drive scanning multithreading
* Column to display file owner
* Column to distinguish logical versus physical allocation
* Built-in alternate languages translations
* Shell menu entry (legacy menu only)
* Numerous bug fixes
    
## Breaking Changes / Removed Features
* Non-default settings from 1.x will have to be set again
* Removed Files pseudo folder
* Removed option to email owner
* Removed help files
