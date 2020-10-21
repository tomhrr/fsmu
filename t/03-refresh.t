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

use Test::More tests => 3;

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

    # Confirm that refresh works as expected.

    my $query_dir = $mount_dir.'/from:user@example.org '.
                    'and not to:asdf4@example.net';
    mkdir $query_dir;
    my $entity = make_message('user@example.org', 'asdf',
                              'asdf', 'asdf data');
    write_message($entity, $dir.'/asdf/asdf1/cur');
    system($refresh_cmd);
    my @query_files = ();
    system("cat '$query_dir/.refresh'");
    find(sub { push @query_files, $File::Find::name },
         $query_dir);
    my @cur_files = grep { /\/cur\/\d/ } @query_files;
    is(@cur_files, 81, "Found 81 'cur' files");

    my $entity2 = make_message('user@example.org', 'asdf',
                               'asdf', 'asdf data data');
    write_message($entity2, $dir.'/asdf/asdf1/cur');
    system($refresh_cmd);
    @query_files = ();
    system("cat '$query_dir/.refresh'");
    find(sub { push @query_files, $File::Find::name },
         $query_dir);
    @cur_files = grep { /\/cur\/\d/ } @query_files;
    is(@cur_files, 82, "Found 82 'cur' files");
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
