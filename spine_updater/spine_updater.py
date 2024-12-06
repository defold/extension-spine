# This script processes all `.spinejson` files in a specified folder, checks their Spine version, 
# and converts them to the specified target version if necessary. 

# Workflow:
# 1. The script takes two arguments:
#    - The input folder containing `.spinejson` files.
#    - The target Spine version (e.g., 4.2.37).
#
# 2. The script scans the folder and subfolders for `.spinejson` files.
#
# 3. For each `.spinejson` file:
#    - Reads and parses the `.spinejson` file as JSON to determine its Spine version.
#    - If the Spine version matches the target version, the file is skipped.
#    - If the Spine version differs:
#      - Renames the `.spinejson` file to `.json` for further processing.
#      - Checks if a `.spine` project file with the same name already exists:
#         - If the `.spine` file exists, skips creating a new `.spine` file.
#         - Otherwise, generates a `.spine` file using the Spine CLI and the original `.json`.
#      - Converts the `.spine` file back to a `.spinejson` file using the target Spine version.
#      - If the script created the `.spine` file, it removes it after the conversion.
#
# 4. During the final export step:
#    - The script ensures `export.json` exists in the script's directory and uses it for the export configuration.
#    - The `.spinejson` file is exported to its original directory without creating additional folders.
#
# 5. Cleanup:
#    - Removes temporary `.json` files created during the process.
#    - Removes `.spine` files if they were created by the script.
#
# 6. Logs:
#    - Prints detailed logs for each `.spinejson` file being processed, including paths and any errors encountered.
#    - Outputs a list of successfully converted files at the end.
#
# Usage:
# Run the script using the following command:
# python script.py <input_folder> <target_version>
#
# - `<input_folder>`: Path to the folder containing `.spinejson` files.
# - `<target_version>`: The desired Spine version for the conversion (e.g., 4.2.37).
#
# Example:
# python script.py /path/to/spinejson/files 4.2.37
#
# Requirements:
# - Spine CLI should be installed and available at the specified path.
# - Ensure `export.json` exists in the script's directory, or the script will warn and proceed to create files without it.
#
# Behavior:
# - The script will process each `.spinejson` file in the folder hierarchy.
# - It avoids redundant work by skipping `.spine` file creation if they already exist.
# - It ensures the `.spinejson` files are exported to their original locations.
# - Temporary files created during the process are cleaned up to avoid clutter.

import os
import json
import subprocess
import sys

def process_spine_files(input_folder, target_version, spine_cli_path):
    """
    Processes all spinejson files in the specified folder, checking their version
    and converting them if necessary. Uses export.json for the last file if present.

    :param input_folder: Path to the folder containing spinejson files
    :param target_version: Target Spine version to check against
    :param spine_cli_path: Path to the Spine CLI executable
    """
    if not os.path.exists(input_folder):
        print(f"Error: The folder '{input_folder}' does not exist.")
        return
    
    export_json_path = os.path.join(os.path.dirname(__file__), "export.json")
    export_json_exists = os.path.exists(export_json_path)

    if not export_json_exists:
        print("Warning: 'export.json' does not exist. A new file will be created.")

    converted_files = []

    for root, _, files in os.walk(input_folder):
        for file in files:
            if file.endswith('.spinejson'):
                spinejson_path = os.path.join(root, file)
                print(f"Processing spinejson path: {spinejson_path}")
                
                # Read and parse the spinejson file
                try:
                    with open(spinejson_path, 'r') as f:
                        data = json.load(f)
                except Exception as e:
                    print(f"Error reading file '{spinejson_path}': {e}")
                    continue

                # Extract Spine version
                file_version = data.get("skeleton", {}).get("spine")
                if not file_version:
                    print(f"Skipping '{spinejson_path}': Unable to determine Spine version.")
                    continue

                if file_version == target_version:
                    # Skip if versions match
                    continue

                print(f"Converting '{spinejson_path}' from version {file_version} to {target_version}...")

                # Generate paths for intermediate files
                json_path = os.path.splitext(spinejson_path)[0] + ".json"
                spine_project_path = os.path.splitext(spinejson_path)[0] + ".spine"
                output_directory = os.path.dirname(spinejson_path)

                print(f"Intermediate paths:\n  JSON Path: {json_path}\n  Spine Path: {spine_project_path}\n  Output Directory: {output_directory}")

                created_spine = False  # Track if the script created the .spine file

                # Check if the .spine file already exists
                if os.path.exists(spine_project_path):
                    print(f"'{spine_project_path}' already exists. Skipping conversion from .spinejson to .spine.")
                else:
                    # Rename spinejson to json
                    os.rename(spinejson_path, json_path)

                    try:
                        # Step 5.2: Convert to .spine using original version
                        result = subprocess.run(
                            [spine_cli_path, "-u", file_version, "--input", json_path, "--output", spine_project_path, "--import"],
                            capture_output=True,
                            text=True
                        )
                        if result.returncode != 0:
                            print(f"Error during conversion of '{spinejson_path}' to .spine:")
                            print("STDOUT:", result.stdout)
                            print("STDERR:", result.stderr)
                            continue
                        created_spine = True
                    except Exception as e:
                        print(f"Unexpected error during conversion of '{spinejson_path}' to .spine: {e}")
                        continue
                    finally:
                        # Clean up the .json file
                        if os.path.exists(json_path):
                            os.remove(json_path)

                # Step 5.3: Export to .spinejson using target version
                print(f"Exporting to spinejson in directory: {output_directory}")
                try:
                    result = subprocess.run(
                        [spine_cli_path, "-u", target_version, "--input", spine_project_path, "--output", output_directory, "--export", export_json_path],
                        capture_output=True,
                        text=True
                    )
                    if result.returncode != 0:
                        print(f"Error during export of '{spine_project_path}' to .spinejson:")
                        print("STDOUT:", result.stdout)
                        print("STDERR:", result.stderr)
                        continue

                    # Record converted file
                    converted_files.append(spinejson_path)
                except Exception as e:
                    print(f"Unexpected error during export of '{spine_project_path}' to .spinejson: {e}")
                    continue
                finally:
                    # Remove the .spine file if it was created by this script
                    if created_spine and os.path.exists(spine_project_path):
                        print(f"Removing temporary spine file: {spine_project_path}")
                        os.remove(spine_project_path)

    # Output converted files
    if converted_files:
        print("\nConverted Files:")
        for file in converted_files:
            print(file)
    else:
        print("No files were converted.")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python script.py <input_folder> <target_version>")
        sys.exit(1)
    
    input_folder = sys.argv[1]
    target_version = sys.argv[2]
    spine_cli_path = "/Applications/Spine.app/Contents/MacOS/Spine"

    process_spine_files(input_folder, target_version, spine_cli_path)