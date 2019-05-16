SConscript(['galerautils/SConscript',
            'gcache/SConscript',
            'gcomm/SConscript',
            'gcs/SConscript',
            'galera/SConscript',
            'garb/SConscript'])

Import('env', 'sysname', 'has_version_script', 'galera_script')

# Clone the environment as it will be extended for this specific library
env = env.Clone()

if has_version_script:
    # Limit symbols visible from Galera DSO.
    # Doing this allows to:
    # - make the ABI more clean and concise
    # - hide symbols from commonly used libraries (boost, asio, etc.), which
    #   binds calls inside the DSO to its own versions of these libraries
    # See: https://akkadia.org/drepper/dsohowto.pdf (section 2.2.5)
    env.Append(SHLINKFLAGS = ' -Wl,--version-script=' + galera_script)

libmmgalera_objs = env['LIBGALERA_OBJS']
libmmgalera_objs.extend(env['LIBMMGALERA_OBJS'])

if sysname == 'darwin':
    galera_lib = env.SharedLibrary('galera_smm', libmmgalera_objs, SHLIBSUFFIX='.so')
else:
    galera_lib = env.SharedLibrary('galera_smm', libmmgalera_objs)

if has_version_script:
    env.Depends(galera_lib, galera_script)

def check_dynamic_symbols(target, source, env):
    import subprocess

    # Check if objdump exists
    p = subprocess.Popen(['objdump', '--version'], stdout=subprocess.PIPE)
    p.wait()
    if p.returncode != 0:
        print('objdump utility is not found. Skipping checks...')
        return 0

    # Check that DSO doesn't contain asio-related dynamic symbols
    if env.Execute(Action(['! objdump -T ' + target[0].abspath + ' | grep asio'], None)):
        return 1
    return 0

env.AddPostAction(galera_lib, Action(check_dynamic_symbols,
                  'Checking dynamic symbols for \'$TARGET\'...'))
