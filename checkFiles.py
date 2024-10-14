def are_files_equal(file1_path, file2_path):
    with open(file1_path, 'r') as file1, open(file2_path, 'r') as file2:
        for line1, line2 in zip(file1, file2):
            if line1 != line2:
                return False
    return True

# Example usage
file1 = 'input.txt'
file2 = 'downloads/input.txt'

if are_files_equal(file1, file2):
    print("The files are equal.")
else:
    print("The files are not equal.")