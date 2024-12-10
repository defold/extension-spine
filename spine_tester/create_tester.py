import os
import json
import glob
import os
import subprocess
import sys

# Ensure configparser is installed
try:
    import configparser
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "configparser"])
    import configparser  # Retry import after installation

def extract_animations_from_spine_json(spine_json_path):
    """
    Reads the given spine json file and extracts the animation names from it.
    """
    try:
        with open(spine_json_path, 'r') as spine_json_file:
            spine_data = json.load(spine_json_file)
            animations = list(spine_data.get("animations", {}).keys())
            return animations
    except FileNotFoundError:
        print(f"Error: {spine_json_path} not found.")
        return []
    except json.JSONDecodeError:
        print(f"Error: Could not parse JSON from {spine_json_path}.")
        return []

def find_spinescene_files():
    # Get the current directory from where the script is run
    current_directory = os.getcwd()

    # Initialize a list to store the relative file paths
    spinescene_files = []

    # Walk through the directory tree
    for root, dirs, files in os.walk(current_directory):
        for file in files:
            if file.endswith(".spinescene"):
                # Create the relative path
                relative_path = os.path.relpath(os.path.join(root, file), current_directory)
                relative_path_with_slash = "/" + relative_path.replace("\\", "/")

                # Check if the file path includes the ignored paths
                if (
                    "assets/template/template.spinescene" not in relative_path_with_slash and
                    "editor/resources/templates/template.spinescene" not in relative_path_with_slash
                ):
                    spinescene_files.append(relative_path_with_slash)

    # Print the search results
    if spinescene_files:
        print("Search for spine scenes:")
        for file in spinescene_files:
            print(file)

    # Path to the output file for generated embedded components
    go_output_file_path = os.path.join(current_directory, "spine_tester", "generated", "go.go")

    # Ensure the output directory exists
    os.makedirs(os.path.dirname(go_output_file_path), exist_ok=True)

    # Dictionary to store component URLs and their animations
    component_animations = {}
    # Dictionary to track file name counts
    file_name_count = {}

    # Write the generated embedded_component text to the go.go file
    with open(go_output_file_path, "w") as go_file:
        print("\nGenerate Components:")
        if spinescene_files:
            for spinescene_file in spinescene_files:
                file_name = os.path.basename(spinescene_file).replace(".spinescene", "")

                # If the file name already exists, append an index to the file name
                original_file_name = file_name
                index = file_name_count.get(original_file_name, 0)
                while file_name in file_name_count:
                    index += 1
                    file_name = f"{original_file_name}_{index}"
                file_name_count[original_file_name] = index
                file_name_count[file_name] = 0  # Ensure the new unique name is tracked

                component_url = f"/go#{file_name}"

                # Read the spinescene file to find the spine_json path
                spinescene_file_path = os.path.join(current_directory, spinescene_file.lstrip("/"))
                try:
                    with open(spinescene_file_path, "r") as spinescene:
                        for line in spinescene:
                            if "spine_json:" in line:
                                spine_json_path = line.split('"')[1]  # Extract the path inside the quotes
                                spine_json_full_path = os.path.join(current_directory, spine_json_path.lstrip("/"))

                                # Extract animations from the spine_json file
                                animations = extract_animations_from_spine_json(spine_json_full_path)

                                # Add to the Lua table dictionary
                                component_animations[component_url] = animations

                                # Use the first animation as the default
                                default_animation = animations[0] if animations else ""

                                # Write the embedded component
                                go_file.write('embedded_components {\n')
                                go_file.write(f'  id: "{file_name}"\n')
                                go_file.write('  type: "spinemodel"\n')
                                go_file.write(f'  data: "spine_scene: \\"{spinescene_file}\\"\\n"\n')
                                go_file.write(f'  "default_animation: \\"{default_animation}\\"\\n"\n')
                                go_file.write('  "skin: \\"\\"\\n"\n')
                                go_file.write('  "material: \\"/defold-spine/assets/spine.material\\"\\n"\n')
                                go_file.write('  ""\n')
                                go_file.write('}\n\n')  # Double newline for readability

                                # Print the component and its animations
                                animations_str = ', '.join(animations)
                                print(f"{component_url} -> {animations_str}")
                except FileNotFoundError:
                    print(f"Error: {spinescene_file_path} not found.")
        else:
            print("No .spinescene files found.")

    print(f"\nGenerated embedded components have been written to {go_output_file_path}")

    # Path to the output file for Lua table
    lua_output_file_path = os.path.join(current_directory, "spine_tester", "generated", "data.lua")

    # Write the Lua table to data.lua
    with open(lua_output_file_path, "w") as lua_file:
        lua_file.write("local M = {\n")
        for component_url, animations in component_animations.items():
            animations_str = ', '.join(f'"{anim}"' for anim in animations)
            lua_file.write(f'  ["{component_url}"] = {{{animations_str}}},\n')
        lua_file.write("}\n")
        lua_file.write("return M\n")

    print(f"Lua table with animations has been written to {lua_output_file_path}")

import configparser
import os

class CustomConfigParser(configparser.ConfigParser):
    """Custom ConfigParser to add spaces around '=' when writing."""
    def write(self, fp, space_around_delimiters=True):
        """Write an .ini-format representation of the configuration state."""
        if space_around_delimiters:
            delimiter = " = "
        else:
            delimiter = "="

        for section in self._sections:
            fp.write(f"[{section}]\n")
            for key, value in self._sections[section].items():
                if value is not None:
                    value = str(value).replace("\n", "\n\t")
                fp.write(f"{key}{delimiter}{value}\n")
            fp.write("\n")

def modify_game_project(file_path):
    if not os.path.isfile(file_path):
        print(f"Error: The file '{file_path}' does not exist.")
        return

    # Load the existing game.project file
    config = CustomConfigParser(allow_no_value=True, delimiters=('='))
    config.optionxform = str  # Preserve case sensitivity
    config.read(file_path)

    # Ensure [project] section exists and add IMGUI dependency if not present
    if "project" not in config:
        config["project"] = {}
    dependencies = [key for key in config["project"] if key.startswith("dependencies#")]
    imgui_dependency = "https://github.com/britzl/extension-imgui/archive/refs/heads/master.zip"
    if imgui_dependency not in (config["project"].get(dep, "") for dep in dependencies):
        new_dep_key = f"dependencies#{len(dependencies)}"
        config["project"][new_dep_key] = imgui_dependency

    # Replace Bootstrap->Main Collection
    if "bootstrap" not in config:
        config["bootstrap"] = {}
    config["bootstrap"]["main_collection"] = "/spine_tester/tester.collection"

    # Replace Input->Game Binding
    if "input" not in config:
        config["input"] = {}
    config["input"]["game_binding"] = "/imgui/bindings/imgui.input_binding"

    # Ensure spine->max_count exists and is set to 8192
    if "spine" not in config:
        config["spine"] = {}
    config["spine"]["max_count"] = "8192"

    # Save the updated file with spaces around the '='
    with open(file_path, "w") as configfile:
        config.write(configfile, space_around_delimiters=True)

def delete_project_files():
    # Define file patterns to delete and track deleted file counts
    file_patterns = ["*.collection", "*.go", "*.gui", "hooks.editor_script"]
    deleted_counts = {pattern: 0 for pattern in file_patterns}

    # Loop through patterns and delete matching files
    for pattern in file_patterns:
        for file_path in glob.glob(os.path.join(".", "**", pattern), recursive=True):
            try:
                os.remove(file_path)
                deleted_counts[pattern] += 1
            except Exception as e:
                print(f"Error deleting {file_path}: {e}")

    # Log the counts of deleted files
    for pattern, count in deleted_counts.items():
        print(f"{pattern}: {count} files deleted")

# Run the function
if __name__ == "__main__":
    # Check if the first argument is "cleanup"
    if len(sys.argv) > 1 and sys.argv[1].lower() == "cleanup":
        delete_project_files()
    # Path to the game.project file in the parent folder
    game_project_path = os.path.join("game.project")
    modify_game_project(game_project_path)
    find_spinescene_files()
