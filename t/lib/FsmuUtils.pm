package FsmuUtils;

use warnings;
use strict;

use autodie;
use File::Slurp qw(read_file write_file);
use File::Temp qw(tempdir);
use MIME::Entity;
use Sys::Hostname;

use base qw(Exporter);
our @EXPORT_OK = qw(make_maildir
                    make_root_maildir
                    make_message
                    write_message
                    mu_init
                    mu_cmd);

my $counter = 1;

sub make_message
{
    my ($from, $to, $subject, $content) = @_;

    my $entity = MIME::Entity->build(
        From    => $from,
        To      => $to,
        Subject => $subject,
        Data    => $content
    );
    return $entity;
}

sub write_message
{
    my ($entity, $dir) = @_;

    my $fn = time().'.'.$$.'_'.$counter++.'.'.hostname();
    open my $fh, '>', $dir.'/'.$fn;
    $entity->print($fh);
    close $fh;
}

my @subdirs = qw(cur new tmp);

sub make_maildir
{
    my ($dir, $name, $dirs, $messages) = @_;

    $dirs ||= 5;
    $messages ||= 9;

    my $wdir = `pwd`;
    chomp $wdir;
    chdir $dir;
    mkdir $name;
    chdir $name;

    for my $n (1..$dirs) {
        my $dir = "asdf".$n;
        mkdir $dir;
        chdir $dir;
        for my $subdir (@subdirs) {
            mkdir $subdir;
        }
        for my $m (1..$messages) {
            my $entity = make_message('user@example.org',
                                      $dir.'@example.net',
                                      "$dir message $m",
                                      "data");
            my $sdir = $subdirs[$m % 2];
            write_message($entity, $sdir);
        }
        chdir "..";
    }

    chdir $wdir;
}

sub make_root_maildir
{
    my ($dirs, $messages) = @_;

    my $dir = tempdir(UNLINK => 1);
    for my $name (qw(asdf qwer zxcv tyui ghjk)) {
        make_maildir($dir, $name, $dirs, $messages);
    }
    return $dir;
}

sub mu_init
{
    my ($dir) = @_;

    my $muhome = tempdir(UNLINK => 1);
    my $args = "--muhome=$muhome";
    my $args2 = "$args --maildir=$dir";
    my $res = system("mu init $args2 >/dev/null 2>&1");
    if ($res != 0) {
        die "Unable to run mu init";
    }
    $res = system("mu index $args >/dev/null 2>&1");
    if ($res != 0) {
        die "Unable to run mu index";
    }

    return $muhome;
}

sub mu_cmd
{
    my ($muhome, $cmd) = @_;

    my $res = system("mu $cmd --muhome=$muhome >/dev/null 2>&1");
    if ($res != 0) {
        die "Unable to run mu $cmd";
    }
}

1;
