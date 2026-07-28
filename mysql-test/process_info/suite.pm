package My::Suite::ProcessInfo;

@ISA = qw(My::Suite);

return "No process_info plugin" unless $ENV{PROCESS_INFO_SO};

sub is_default { 1 }

bless { };
