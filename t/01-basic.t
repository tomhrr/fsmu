#!/usr/bin/perl

use warnings;
use strict;

use lib './t/lib';
use FsmuUtils qw(make_root_maildir
                 make_message
                 write_message
                 mu_init
                 mu_cmd);
use autodie;
use File::Basename;
use File::Find;
use File::Slurp qw(read_file);
use File::Temp qw(tempdir);
use List::Util qw(first);

use Test::More tests => 22;

my $mount_dir;
my $pid;

{
    my @help = `./fsmu --help`;
    like($help[0], qr/^usage/, 'Got help details');

    my $dir = make_root_maildir();
    my $muhome = mu_init($dir);
    my $backing_dir = tempdir(UNLINK => 1);
    $mount_dir = tempdir(UNLINK => 1);
    my $refresh_cmd = "mu index --muhome=$muhome >/dev/null";
    if ($pid = fork()) {
        sleep(1);
    } else {
        my $res = system("./fsmu -s --muhome=$muhome ".
                         "--delete-remove --backing-dir=$backing_dir ".
                         "$mount_dir");
        sleep(3600);
        exit();
    }

    # Confirm basic query returns results.
    
    my $query_dir = $mount_dir.'/from:user@example.org '.
                    'and not to:asdf4@example.net';
    mkdir $query_dir;
    my @query_files;
    find(sub { push @query_files, $File::Find::name },
         $query_dir);
    my @new_files = grep { /\/new\/\d/ } @query_files;
    is(@new_files, 60, "Found 60 'new' files");
    my @cur_files = grep { /\/cur\/\d/ } @query_files;
    is(@cur_files, 60, "Found 60 'cur' files");

    # Confirm that mail items can be read.

    my $data = read_file($cur_files[0]);
    ok($data, 'Able to read mail file from mount directory');

    # Confirm that raw backing directories are not included in the
    # mount directory.

    my @files;
    find(sub { push @files, $File::Find::name },
         $mount_dir);
    ok((not grep { /\/_/ } @files),
        'No raw backing directories in mount directory');

    # Add another mail item, confirm that requerying picks it up.

    my $entity = make_message('user@example.org', 'asdf',
                              'asdf', 'asdf data');
    write_message($entity, $dir.'/asdf/asdf1/cur');
    system($refresh_cmd);
    @query_files = ();
    rmdir $query_dir;
    mkdir $query_dir;
    find(sub { push @query_files, $File::Find::name },
         $query_dir);
    @cur_files = grep { /\/cur\/\d/ } @query_files;
    is(@cur_files, 61, "Found 61 'cur' files");

    # Rename mail item within original directory.

    my $cur_file = $cur_files[0];
    my $new_cur_file = $cur_file.':2,S';
    eval { rename $cur_file, $new_cur_file };
    my $info = $!;
    ok((not $@), "Renamed file within existing directory");
    diag $info if $info;
    ok((not -e $cur_file),
        "Original file in maildir no longer exists");
    ok((-e $new_cur_file),
        "New file in maildir exists");
    my $basename = basename($cur_file);
    my $new_basename = basename($new_cur_file);
    ok((not -e $query_dir.'/cur/'.$basename),
        "Original file in query dir no longer exists");
    ok((-e $query_dir.'/cur/'.$new_basename),
        "New file in query dir exists");

    # Rename mail item into different directory (first level up).

    my $new_file = $new_files[0];
    my $new_new_file = $new_file;
    $new_new_file =~ s/\/new\//\/cur\//;
    $! = undef;
    eval { rename $new_file, $new_new_file };
    $info = $!;
    ok((not $@), "Renamed file into different directory");
    diag $info if $info;
    ok((not -e $new_file),
        "Original file in maildir no longer exists");
    ok((-e $new_new_file),
        "New file in maildir exists");
    $basename = basename($new_file);
    ok((not -e $query_dir.'/new/'.$basename),
        "Original file in query dir no longer exists");
    ok((-e $query_dir.'/cur/'.$new_basename),
        "New file in query dir exists");

    # Confirm query directly to cur/new works.

    my $query_dir2 = $mount_dir.'/to:asdf4@example.net';
    mkdir $query_dir2;
    @query_files = ();
    find(sub { push @query_files, $File::Find::name },
         $query_dir2.'/cur');
    is(@query_files, 16, "Found 16 'cur' files");

    # Deletion carries through to the mailbox.

    my @all_files;
    find(sub { push @all_files, $File::Find::name }, $dir);
    my $res = unlink $query_files[1];
    ok($res, 'Deleted file from search directory');
    my @all_files2;
    find(sub { push @all_files2, $File::Find::name }, $dir);
    is(@all_files, (@all_files2 + 1),
        'Delete carries through to mailbox');

    # Permit rmdir on a query directory.

    $res = rmdir $query_dir2;
    ok($res, 'Able to call rmdir on query directory');
    ok((-e $backing_dir.'/_to:asdf4@example.net'),
        'Backing directory left in place');

    # Confirm maildir queries work correctly.

    my $query_dir3 = $mount_dir.'/maildir:+asdf+asdf4';
    mkdir $query_dir3;
    @query_files = ();
    find(sub { push @query_files, $File::Find::name },
         $query_dir3.'/cur');
    is(@query_files, 4, "Found 4 'cur' files");
}

END {
    if ($mount_dir) {
        system("fusermount -u $mount_dir");
    }
    if ($pid) {
        kill('TERM', $pid);
        waitpid($pid, 0);
    }
    exit(0);
}

1;
