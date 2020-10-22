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
use Digest::MD5;
use File::Basename;
use File::Find;
use File::Slurp qw(read_file);
use File::Temp qw(tempdir);
use List::Util qw(first);

use Test::More tests => 34;

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

    my $get_md5 = sub {
        my ($path) = @_;
        open my $fh, '<', $path;
        my $md5 = Digest::MD5->new();
        my $ctx = $md5->addfile($fh);
        my $digest = $ctx->hexdigest();
        close $fh;
        return $digest;
    };

    my $get_maildir_path = sub {
        my ($query_dir_path) = @_;
        my $basename = basename($query_dir_path);
        $basename =~ s/^\d+_//;
        my $md5 = $get_md5->($query_dir_path);
        my @matches;
        find(sub {
            my $path = $File::Find::name;
            if (-f $path) {
                my $path_md5 = $get_md5->($path);
                if ($md5 eq $path_md5) {
                    push @matches, $path;
                }
            }
        }, $dir);
        if (@matches > 1) {
            @matches = grep { /$basename/ } @matches;
        }
        if (@matches > 1) {
            die "Multiple matches found";
        }
        return $matches[0];
    };

    # Confirm basic query returns results.

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
    is(@cur_files, 81, "Found 81 'cur' files");

    # Rename mail item within original directory.

    my $cur_file = $cur_files[0];
    my $md_cur_file = $get_maildir_path->($cur_file);
    my $new_cur_file = $cur_file.':2,S';
    $! = undef;
    eval { rename $cur_file, $new_cur_file };
    my $info = $!;
    ok((not $@), "Renamed file within existing directory");
    diag $info if $info;
    ok((not -e $cur_file),
        "Original file in query dir no longer exists");
    ok((-e $new_cur_file),
        "New file in query dir exists");
    my $md_new_cur_file = $get_maildir_path->($new_cur_file);
    ok((not -e $md_cur_file),
        "Original file in maildir no longer exists");
    ok((-e $md_new_cur_file),
        "New file in maildir exists");

    # Confirm rename affected only the flags in the maildir path.

    my $md_basename = basename($md_cur_file);
    my $md_new_basename = basename($md_new_cur_file);
    $md_basename =~ s/(.*):.*/$1/;
    $md_new_basename =~ s/(.*):.*/$1/;
    is($md_basename, $md_new_basename,
        'Original maildir path unaffected except for flag change');

    # Rename mail item within original directory, with new name.

    $cur_file = $cur_files[1];
    $new_cur_file = $cur_file;
    $new_cur_file =~ s/(.*)\/.*/$1\/new-path/;
    $! = undef;
    eval { rename $cur_file, $new_cur_file };
    $info = $!;
    ok((not $@), "Renamed file within existing directory (new name)");
    diag $info if $info;
    ok((not -e $cur_file),
        "Original file in query dir no longer exists");
    ok((-e $new_cur_file),
        "New file in query dir exists");
    $md_new_cur_file = $get_maildir_path->($new_cur_file);
    ok((not -e $md_cur_file),
        "Original file in maildir no longer exists");
    ok((-e $md_new_cur_file),
        "New file in maildir exists");
    $md_new_basename = basename($md_new_cur_file);
    is($md_new_basename, 'new-path',
        'New name used in maildir path');

    # Rename mail item into different directory (first level up).

    my $new_file = $new_files[0];
    my $new_new_file = $new_file;
    $new_new_file =~ s/\/new\//\/cur\//;
    $! = undef;
    eval { rename $new_file, $new_new_file };
    $info = $!;
    ok((not $@), "Renamed file into different directory");
    diag $@ if $@;
    diag $info if $info;
    ok((not -e $new_file),
        "Original file in maildir no longer exists");
    ok((-e $new_new_file),
        "New file in maildir exists");

    # Confirm maildir queries work correctly.

    my $query_dir3 = $mount_dir.'/maildir:+asdf+asdf4';
    mkdir $query_dir3;
    @query_files = ();
    find(sub { push @query_files, $File::Find::name },
         $query_dir3.'/cur');
    is(@query_files, 5, "Found 5 'cur' files");

    # Confirm query directly to cur/new works.

    my $query_dir2 = $mount_dir.'/to:asdf4@example.net';
    mkdir $query_dir2;
    @query_files = ();
    find(sub { push @query_files, $File::Find::name },
         $query_dir2.'/cur');
    is(@query_files, 21, "Found 21 'cur' files");

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
    ok((not -e $backing_dir.'/_to:asdf4@example.net'),
        'Backing directory removed');

    # Set up two query directories that overlap.  Confirm that
    # movement in one causes updates in the other.

    my $query_dir4 = $mount_dir.'/from:user@example.org';
    mkdir $query_dir4;
    my $query_dir5 = $mount_dir.'/data';
    mkdir $query_dir5;

    my @query_files4 = ();
    find(sub { push @query_files4, $File::Find::name },
         $query_dir4);
    my @query_files4_nodir =
        sort
        map { my $b = $_; $b =~ s/$query_dir4//; $b }
            @query_files4;
    my @query_files5 = ();
    find(sub { push @query_files5, $File::Find::name },
         $query_dir5);
    my @query_files5_nodir =
        sort
        map { my $b = $_; $b =~ s/$query_dir5//; $b }
            @query_files5;

    ok(@query_files4_nodir,
        'First query has results');
    ok(@query_files5_nodir,
        'Second query has results');
    is_deeply(\@query_files4_nodir, \@query_files5_nodir,
        'Query directories cover the same files');

    $new_file = first { /\/new\// } @query_files4;
    $new_new_file = $new_file;
    $new_new_file =~ s/\/new\//\/cur\//;
    $! = undef;
    eval { rename $new_file, $new_new_file };
    $info = $!;
    ok((not $@), "Renamed file into different directory");
    diag $@ if $@;
    diag $info if $info;
    ok((not -e $new_file),
        "Original file in maildir no longer exists");
    ok((-e $new_new_file),
        "New file in maildir exists");

    @query_files4 = ();
    find(sub { push @query_files4, $File::Find::name },
         $query_dir4);
    @query_files4_nodir =
        sort
        map { my $b = $_; $b =~ s/$query_dir4//; $b }
            @query_files4;
    @query_files5 = ();
    find(sub { push @query_files5, $File::Find::name },
         $query_dir5);
    @query_files5_nodir =
        sort
        map { my $b = $_; $b =~ s/$query_dir5//; $b }
            @query_files5;

    is_deeply(\@query_files4_nodir, \@query_files5_nodir,
        'Query directories still cover the same files');
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
