use strict;
use warnings;
use Getopt::Long;
use File::Path;
use File::Find;
use File::Spec;
use Cwd;

# Command-line options:
my $input_path  = '';
my $output_file = '';  # This will be the header file (e.g., perlpack.h)
my $prefix      = '';  # The virtual prefix (for example, "/my/perl")
my $ld          = 'ld';
my $skip        = '';

GetOptions(
    'input-path|i=s'  => \$input_path,
    'output-file|o=s' => \$output_file,
    'prefix=s'        => \$prefix,
    'ld=s'            => \$ld,
    'skip=s'          => \$skip,
) or die "Usage: $0 -i <input_path> -o <output_file> -prefix <virtual_prefix>\n";

die "Input path does not exist or is not a directory\n" unless -d $input_path;
die "Output file not specified\n" if $output_file eq '';
die "Prefix not specified\n" if $prefix eq '';

# Create a directory for intermediate object files.
my $obj_dir = $output_file . ".o";
File::Path::make_path($obj_dir);

my $oldcwd = Cwd::getcwd();
my (@objects, @files, @safepaths, @relpaths);

# Recursively scan the input directory.
File::Find::find(sub {
    # Process only files (skip directories).
    return if -d;
    # Skip files if a regex is provided.
    return if ($skip ne '' && $_ =~ /$skip/);
    
    my $p = $File::Find::name;
    push @files, $p;
    
    # Create a “safe” symbol name: replace /, . and - with _
    (my $safepath = $p) =~ s/[\/\.\-]/_/g;
    push @safepaths, $safepath;
    
    # Create a relative path (what Perl will see) relative to $input_path.
    my $relpath = File::Spec->abs2rel($p, $input_path);
    push @relpaths, $relpath;
    
    # Create an object file for this file.
    my $obj_file = File::Spec->catfile($obj_dir, $safepath . ".o");
    push @objects, $obj_file;
    # Use ld to create a relocatable object containing the file's binary contents.
    system($ld, '-r', '-b', 'binary', '-o', $obj_file, $p) == 0
      or die "ld command failed on $p: $?";
}, $input_path);

# Generate the header file.
open my $fh, '>', $output_file or die "Cannot open $output_file: $!";
print $fh "#ifndef PERLPACK_H\n#define PERLPACK_H\n\n";
# Define the virtual filesystem root.
print $fh "#define PACKFS_BUILTIN_PREFIX \"$prefix\"\n\n";
print $fh "size_t packfs_builtin_files_num = ", scalar(@files), ";\n\n";

# The absolute paths are built by concatenating the virtual prefix with the relative path.
print $fh "const char* packfs_builtin_abspaths[] = {\n  \"",
      join("\",\n  \"", map { File::Spec->catfile($prefix, $_) } @relpaths),
      "\"\n};\n\n";

# Print the safe symbol names.
print $fh "const char* packfs_builtin_safepaths[] = {\n  \"",
      join("\",\n  \"", @safepaths),
      "\"\n};\n\n";

# Declare external symbols for each file.
foreach my $safepath (@safepaths) {
    print $fh "extern const char _binary_${safepath}_start[];\n";
    print $fh "extern const char _binary_${safepath}_end[];\n";
}
print $fh "\n";

# Define arrays of pointers to the file data.
print $fh "const char* packfs_builtin_starts[] = {\n  ",
      join(",\n  ", map { "_binary_${_}_start" } @safepaths),
      "\n};\n\n";
print $fh "const char* packfs_builtin_ends[] = {\n  ",
      join(",\n  ", map { "_binary_${_}_end" } @safepaths),
      "\n};\n\n";
print $fh "#endif\n";
close $fh;

# Also write out a list of the object files (to be used during linking).
open my $ofh, '>', $output_file . ".txt" or die "Cannot open " . $output_file . ".txt: $!";
print $ofh join("\n", @objects);
close $ofh;

print "perlpack.h generated successfully (object file list in $output_file.txt).\n";
