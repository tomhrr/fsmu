#!/usr/bin/perl

use warnings;
use strict;

use lib './t/lib';
use FsmuUtils qw(make_root_maildir
                 make_message
                 write_message
                 mu_init
                 mu_cmd);
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
    my ($muhome, $refresh_cmd) = mu_init($dir);
    my $backing_dir = tempdir(UNLINK => 1);
    $mount_dir = tempdir(UNLINK => 1);
    if ($pid = fork()) {
        sleep(1);
    } else {
        my $res = system("./fsmu --muhome=$muhome ".
                         "--backing-dir=$backing_dir ".
                         "$mount_dir");
        sleep(3600);
        exit();
    }

    # Search for some mail items, and confirm removal does not work.

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

    my $res = unlink($new_files[0]);
    ok((not $res), 'Unable to delete file');
    like($!, qr/Operation not permitted/,
        'Got correct error message');
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
