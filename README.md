zeroperl is an experimental build of Perl5 in a sandboxed, self-contained WebAssembly module.

Read the full blog [here](https://open.substack.com/pub/andrews/p/zeroperl-sandboxed-perl-with-webassembly?r=44njw&utm_campaign=post&utm_medium=web&showWelcomeOnShare=false)


Notes:
[1]: for some reason if `LC_ALL=1` is not passed as an environment variable to Perl, it crashes. [No idea why](https://github.com/Perl/perl5/issues/22375). 
[2]: the first argument passed to Perl must be 'zeroperl'. 
[3]: depending on your runtime, you may need to map /dev/null as a preopen. 
