import os

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

        print("\nGenerate Components:")
        for file in spinescene_files:
            file_name = os.path.basename(file).replace(".spinescene", "")
            print(f"/go#{file_name}")
    else:
        print("No .spinescene files found.")

    # Path to the output file
    output_file_path = os.path.join(current_directory, "spine_tester", "generated", "go.go")

    # Ensure the output directory exists
    os.makedirs(os.path.dirname(output_file_path), exist_ok=True)

    # Write the generated embedded_component text to the file
    with open(output_file_path, "w") as f:
        if spinescene_files:
            for file in spinescene_files:
                file_name = os.path.basename(file).replace(".spinescene", "")
                f.write('embedded_components {\n')
                f.write(f'  id: "{file_name}"\n')
                f.write('  type: "spinemodel"\n')
                f.write(f'  data: "spine_scene: \\"{file}\\"\\n"\n')
                f.write('  "default_animation: \\"\\"\\n"\n')
                f.write('  "skin: \\"\\"\\n"\n')
                f.write('  "material: \\"/defold-spine/assets/spine.material\\"\\n"\n')
                f.write('  ""\n')
                f.write('}\n\n')  # Double newline for readability
        else:
            print("No .spinescene files found.")

    print(f"\nGenerated embedded components have been written to {output_file_path}")

# Run the function
if __name__ == "__main__":
    find_spinescene_files()
