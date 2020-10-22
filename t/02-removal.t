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

use Test::More tests => 5;

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

    # Search for some mail items, and confirm removal updates the
    # reverse directory.

    my $query_dir = $mount_dir.'/from:user@example.org '.
                    'and not to:asdf4@example.net';
    mkdir $query_dir;
    my @query_files;
    find(sub { push @query_files, $File::Find::name },
         $query_dir);
    my @new_files = grep { /\/new\/\d/ } @query_files;
    is(@new_files, 100, "Found 100 'new' files");
    my @cur_files = grep { /\/cur\/\d/ } @query_files;
    is(@cur_files, 80, "Found 80 'cur' files");

    rmdir $query_dir;
    my @files;
    find(sub { push @files, $File::Find::name },
         $backing_dir);
    is(@files, 1,
        'No files in backing directory when query directory removed');

    # Do two overlapping searches, and confirm removal of one reverts
    # all backing directory changes.

    mkdir $query_dir;
    @query_files = ();
    find(sub { push @query_files, $File::Find::name },
         $query_dir);
    @files = ();
    find(sub { push @files, $File::Find::name },
         $backing_dir);
    my $file_count = scalar @files;

    my $query_dir2 = $mount_dir.'/from:user@example.org '.
                     'and not to asdf3@example.net';
    mkdir $query_dir2;
    @query_files = ();
    find(sub { push @query_files, $File::Find::name },
         $query_dir2);
    @files = ();
    find(sub { push @files, $File::Find::name },
         $backing_dir);
    my $file_count2 = scalar @files;

    rmdir $query_dir2;
    @files = ();
    find(sub { push @files, $File::Find::name },
         $backing_dir);
    my $file_count3 = scalar @files;
    is($file_count3, $file_count,
        'Backing directory has previous file count');
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