import sys, os, time, getopt, subprocess, pipes

HOME = os.path.abspath(os.path.join(os.path.dirname(__file__)))


# llama.cpp inference (examples/simple with SimRoiStart/SimRoiEnd around the decode loop).
# Override the paths with the LLAMA_BIN and LLAMA_MODEL environment variables.
LLAMA_BIN = os.getenv('LLAMA_BIN', '/home/derk/llama.cpp/build/bin/llama-simple')
LLAMA_MODEL = os.getenv('LLAMA_MODEL', '/home/derk/models/stories260K.gguf')
# input size -> (prompt, number of generated tokens)
LLAMA_INPUTS = {
  'test':   ('Hello', 1),
  'small':  ('Once upon a time', 1),
  'medium': ('Once upon a time', 100),
}


def allbenchmarks():
    return [ 'pi', 'llama' ]

def allinputs():
  return ('test', 'small', 'medium', 'large')


class Program:

  def benchmark(self, program, inputsize, nthreads):
    if program == 'pi':
      if inputsize == 'test':
        num_steps = '1000'
      elif inputsize == 'small':
        num_steps = '100000'
      elif inputsize == 'large':
        num_steps = '1000000'
      else:
        raise ValueError("Invalid input size %s" % inputsize)
      return '%s/pi/pi %s' % (HOME, num_steps)

    elif program == 'llama':
      if inputsize not in LLAMA_INPUTS:
        raise ValueError("Invalid input size %s for llama (valid: %s)" % (inputsize, ', '.join(sorted(LLAMA_INPUTS))))
      # llama-simple always uses llama.cpp's default of 4 threads, so nthreads is not passed on.
      prompt, n_predict = LLAMA_INPUTS[inputsize]
      return '%s -m %s -n %d %s' % (LLAMA_BIN, LLAMA_MODEL, n_predict, pipes.quote(prompt))

    else:
      raise ValueError("Unknown program %s" % program)


  def __init__(self, program, nthreads, inputsize, benchmark_options = []):
    if program not in allbenchmarks():
      raise ValueError("Invalid benchmark %s" % program)
    if inputsize not in allinputs():
      raise ValueError("Invalid input size %s" % inputsize)
    self.program = program
    self.nthreads = nthreads
    self.inputsize = inputsize
    self.cmd = self.benchmark(self.program, self.inputsize, self.nthreads)


  def ncores(self):
    return self.nthreads


  def run(self, submit):
    print '[LOCAL]', '[========== Running benchmark', self.program, '==========]'
    cmd = submit + ' ' + self.cmd
    print '[LOCAL]', 'Running \'' + cmd + '\':'
    print '[LOCAL]', '[---------- Beginning of output ----------]'
    sys.stdout.flush()
    sys.stderr.flush()
    os.putenv('OMP_NUM_THREADS', str(self.ncores()))
    os.system(cmd)
    print '[LOCAL]', '[----------    End of output    ----------]'
    print '[LOCAL]', 'Done.'

  def rungraphiteoptions(self):
    return ''
