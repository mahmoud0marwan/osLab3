
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <glob.h>
#include <vector>
#include <string>
#include <iostream>
#include "command.h"
#include "tokenizer.h"

static void freeSimpleCommand(SimpleCommand *cmd)
{
    if (cmd == NULL)
        return;

    for (int i = 0; i < cmd->_numberOfArguments; i++)
        free(cmd->_arguments[i]);

    free(cmd->_arguments);
    free(cmd);
}

static char *copyString(const std::string &s)
{
    return strdup(s.c_str());
}

static void setFile(char *&destination, const std::string &name)
{
    if (destination != NULL)
        free(destination);

    destination = copyString(name);
}

static bool isOperator(TokenType type)
{
    return type == TOKEN_PIPE ||
           type == TOKEN_REDIRECT ||
           type == TOKEN_APPEND ||
           type == TOKEN_INPUT ||
           type == TOKEN_ERROR ||
           type == TOKEN_REDIRECT_AND_ERROR ||
           type == TOKEN_BACKGROUND ||
           type == TOKEN_EOF;
}

static void addArgument(SimpleCommand *cmd, const std::string &value)
{
    bool hasWildcard =
        value.find('*') != std::string::npos ||
        value.find('?') != std::string::npos;

    if (hasWildcard)
    {
        glob_t matches;
        memset(&matches, 0, sizeof(matches));

        if (glob(value.c_str(), 0, NULL, &matches) == 0 &&
            matches.gl_pathc > 0)
        {
            for (size_t i = 0; i < matches.gl_pathc; i++)
                cmd->insertArgument(strdup(matches.gl_pathv[i]));

            globfree(&matches);
            return;
        }

        globfree(&matches);
    }

    cmd->insertArgument(copyString(value));
}

static void childSignalHandler(int)
{
    int savedErrno = errno;
    pid_t pid;

    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
    {
        const char message[] = "A child process terminated.\n";

        int fd = open("myshell.log",
                      O_WRONLY | O_CREAT | O_APPEND,
                      0644);

        if (fd >= 0)
        {
            write(fd, message, sizeof(message) - 1);
            close(fd);
        }
    }

    errno = savedErrno;
}
static bool mergeQuotes(const std::vector<Token> &in, std::vector<Token> &out)
{
    for (size_t i = 0; i < in.size(); i++)
    {
        if (in[i].type == TOKEN_EOF)
        {
            out.push_back(in[i]);
            continue;
        }

        Token merged;
        merged.type = in[i].type;

        std::string word;
        char quote = 0;

        while (true)
        {
            const std::string &v = in[i].value;

            for (size_t k = 0; k < v.size(); k++)
            {
                char c = v[k];

                if (quote)
                {
                    if (c == quote)
                        quote = 0;
                    else
                        word += c;
                }
                else if (c == '"' || c == '\'')
                    quote = c;
                else
                    word += c;
            }

            if (!quote)
                break;

            // Quote still open: continue with the next token.
            if (i + 1 >= in.size() || in[i + 1].type == TOKEN_EOF)
            {
                fprintf(stderr, "Error: unterminated quote.\n");
                return false;
            }

            word += ' ';
            i++;
        }

        merged.value = word;
        out.push_back(merged);
    }

    return true;
}
void parse(std::vector<Token> &tokens)
{
    if (tokens.empty())
        return;
    
    std::vector<Token> merged;
    if (!mergeQuotes(tokens, merged))
        return;
    tokens = merged;
    Command currentCommand;
    SimpleCommand *currentSimpleCommand = new SimpleCommand();
    bool invalid = false;

    for (size_t i = 0; i < tokens.size(); i++)
    {
        TokenType type = tokens[i].type;

        if (type == TOKEN_EOF)
            break;

        if (type == TOKEN_COMMAND || type == TOKEN_ARGUMENT)
        {
            addArgument(currentSimpleCommand, tokens[i].value);
        }
        else if (type == TOKEN_PIPE)
        {
            if (currentSimpleCommand->_numberOfArguments == 0)
            {
                fprintf(stderr, "Error: empty command before pipe.\n");
                invalid = true;
                break;
            }

            currentCommand.insertSimpleCommand(currentSimpleCommand);
            currentSimpleCommand = new SimpleCommand();
        }
        else if (type == TOKEN_REDIRECT ||
                 type == TOKEN_APPEND ||
                 type == TOKEN_INPUT ||
                 type == TOKEN_ERROR ||
                 type == TOKEN_REDIRECT_AND_ERROR)
        {
            if (i + 1 >= tokens.size() ||
                tokens[i + 1].type == TOKEN_EOF ||
                isOperator(tokens[i + 1].type))
            {
                fprintf(stderr, "Error: missing redirection filename.\n");
                invalid = true;
                break;
            }

            std::string filename = tokens[i + 1].value;

            if (type == TOKEN_REDIRECT)
            {
                setFile(currentCommand._outFile, filename);
                currentCommand._append = 0;
            }
            else if (type == TOKEN_APPEND)
            {
                setFile(currentCommand._outFile, filename);
                currentCommand._append = 1;
            }
            else if (type == TOKEN_INPUT)
            {
                setFile(currentCommand._inputFile, filename);
            }
            else if (type == TOKEN_ERROR)
            {
                setFile(currentCommand._errFile, filename);
            }
            else if (type == TOKEN_REDIRECT_AND_ERROR)
            {
                setFile(currentCommand._outFile, filename);
                setFile(currentCommand._errFile, filename);
                currentCommand._out_error = 1;
                currentCommand._append = 1;
            }

            i++;
        }
        else if (type == TOKEN_BACKGROUND)
        {
            currentCommand._background = 1;

            if (i + 1 < tokens.size() &&
                tokens[i + 1].type != TOKEN_EOF)
            {
                fprintf(stderr, "Error: '&' must be at the end.\n");
                invalid = true;
                break;
            }
        }
    }

    if (invalid)
    {
        freeSimpleCommand(currentSimpleCommand);
        currentCommand.clear();
        return;
    }

    if (currentSimpleCommand->_numberOfArguments > 0)
    {
        currentCommand.insertSimpleCommand(currentSimpleCommand);
    }
    else
    {
        freeSimpleCommand(currentSimpleCommand);

        if (currentCommand._numberOfSimpleCommands > 0)
        {
            fprintf(stderr, "Error: empty command after pipe.\n");
            currentCommand.clear();
            return;
        }
    }

    if (currentCommand._numberOfSimpleCommands == 0)
    {
        currentCommand.clear();
        return;
    }

    currentCommand.execute();
}

SimpleCommand::SimpleCommand()
{
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;

    _arguments = (char **)malloc(
        _numberOfAvailableArguments * sizeof(char *));

    if (_arguments == NULL)
    {
        perror("malloc");
        exit(1);
    }

    _arguments[0] = NULL;
}

void SimpleCommand::insertArgument(char *argument)
{
    if (_numberOfAvailableArguments == _numberOfArguments + 1)
    {
        _numberOfAvailableArguments *= 2;

        _arguments = (char **)realloc(
            _arguments,
            _numberOfAvailableArguments * sizeof(char *));

        if (_arguments == NULL)
        {
            perror("realloc");
            exit(1);
        }
    }

    _arguments[_numberOfArguments] = argument;
    _arguments[_numberOfArguments + 1] = NULL;
    _numberOfArguments++;
}

Command::Command()
{
    _numberOfAvailableSimpleCommands = 1;
    _numberOfSimpleCommands = 0;

    _simpleCommands = (SimpleCommand **)malloc(
        _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));

    if (_simpleCommands == NULL)
    {
        perror("malloc");
        exit(1);
    }

    _outFile = NULL;
    _inputFile = NULL;
    _errFile = NULL;
    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand)
{
    if (_numberOfAvailableSimpleCommands == _numberOfSimpleCommands)
    {
        _numberOfAvailableSimpleCommands *= 2;

        SimpleCommand **temp = (SimpleCommand **)realloc(
            _simpleCommands,
            _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));

        if (temp == NULL)
        {
            perror("realloc");
            exit(1);
        }

        _simpleCommands = temp;
    }

    _simpleCommands[_numberOfSimpleCommands] = simpleCommand;
    _numberOfSimpleCommands++;
}

void Command::clear()
{
    for (int i = 0; i < _numberOfSimpleCommands; i++)
        freeSimpleCommand(_simpleCommands[i]);

    free(_simpleCommands);
    _simpleCommands = NULL;

    free(_outFile);
    free(_inputFile);
    free(_errFile);

    _outFile = NULL;
    _inputFile = NULL;
    _errFile = NULL;

    _numberOfSimpleCommands = 0;
    _numberOfAvailableSimpleCommands = 0;
    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::print()
{
    printf("\n\n              COMMAND TABLE\n\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        printf("  %-3d ", i);

        for (int j = 0;
             j < _simpleCommands[i]->_numberOfArguments;
             j++)
        {
            printf("\"%s\" \t", _simpleCommands[i]->_arguments[j]);
        }

        printf("\n");
    }

    printf("\n");
    printf("  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");

    printf("  %-12s %-12s %-12s %-12s %-12s\n",
           _outFile ? _outFile : "default",
           _inputFile ? _inputFile : "default",
           _errFile ? _errFile : "default",
           _out_error ? "YES" : "default",
           _background ? "YES" : "NO");

    printf("\n");
}

void Command::execute()
{
    if (_numberOfSimpleCommands == 0)
    {
        clear();
        return;
    }

    print();

    // Built-in command: exit
    if (_numberOfSimpleCommands == 1 &&
        !_background &&
        strcmp(_simpleCommands[0]->_arguments[0], "exit") == 0)
    {
        clear();
        printf("Good bye!!\n");
        exit(0);
    }

    // Built-in command: cd
    if (_numberOfSimpleCommands == 1 &&
        !_background &&
        strcmp(_simpleCommands[0]->_arguments[0], "cd") == 0)
    {
        char *directory = _simpleCommands[0]->_arguments[1];

        if (directory == NULL)
            directory = getenv("HOME");

        if (directory == NULL)
        {
            fprintf(stderr, "cd: HOME is not set.\n");
        }
        else if (chdir(directory) != 0)
        {
            perror("cd");
        }

        clear();
        return;
    }

    int count = _numberOfSimpleCommands;
    std::vector<pid_t> children;
    std::vector<int *> pipes;

    for (int i = 0; i < count - 1; i++)
    {
        int *p = new int[2];

        if (pipe(p) == -1)
        {
            perror("pipe");
            delete[] p;

            for (size_t j = 0; j < pipes.size(); j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
                delete[] pipes[j];
            }

            clear();
            return;
        }

        pipes.push_back(p);
    }

    for (int i = 0; i < count; i++)
    {
        pid_t pid = fork();

        if (pid == -1)
        {
            perror("fork");
            break;
        }

        if (pid == 0)
        {
            // The shell ignores Ctrl+C, but child programs should not.
            signal(SIGINT, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            // Read from the previous command's pipe.
            if (i > 0)
            {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1)
                {
                    perror("dup2");
                    _exit(1);
                }
            }

            // Write to the next command's pipe.
            if (i < count - 1)
            {
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1)
                {
                    perror("dup2");
                    _exit(1);
                }
            }

            // Input redirection applies to the first command.
            if (i == 0 && _inputFile != NULL)
            {
                int fd = open(_inputFile, O_RDONLY);

                if (fd == -1)
                {
                    perror(_inputFile);
                    _exit(1);
                }

                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            // Output and error redirection apply to the last command.
            if (i == count - 1 && _outFile != NULL)
            {
                int flags = O_WRONLY | O_CREAT;

                flags |= _append ? O_APPEND : O_TRUNC;

                int fd = open(_outFile, flags, 0644);

                if (fd == -1)
                {
                    perror(_outFile);
                    _exit(1);
                }

                dup2(fd, STDOUT_FILENO);

                if (_out_error)
                    dup2(fd, STDERR_FILENO);

                close(fd);
            }

            if (i == count - 1 &&
                _errFile != NULL &&
                !_out_error)
            {
                int fd = open(
                    _errFile,
                    O_WRONLY | O_CREAT | O_TRUNC,
                    0644);

                if (fd == -1)
                {
                    perror(_errFile);
                    _exit(1);
                }

                dup2(fd, STDERR_FILENO);
                close(fd);
            }

            // Close all pipe descriptors in the child.
            for (size_t j = 0; j < pipes.size(); j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            execvp(
                _simpleCommands[i]->_arguments[0],
                _simpleCommands[i]->_arguments);

            perror(_simpleCommands[i]->_arguments[0]);
            _exit(127);
        }

        children.push_back(pid);
    }

    // The parent must close its pipe descriptors too.
    for (size_t i = 0; i < pipes.size(); i++)
    {
        close(pipes[i][0]);
        close(pipes[i][1]);
        delete[] pipes[i];
    }

    // Foreground commands wait. Background commands return immediately.
    if (!_background)
    {
        for (size_t i = 0; i < children.size(); i++)
        {
            while (waitpid(children[i], NULL, 0) == -1)
            {
                if (errno == EINTR)
                    continue;

                // SIGCHLD may already have reaped this child.
                break;
            }
        }
    }

    clear();
}

void Command::prompt()
{
    std::string input;

    while (true)
    {
        printf("myshell> ");
        fflush(stdout);

        if (!std::getline(std::cin, input))
        {
            printf("\n");
            break;
        }

        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
    }
}

Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand = NULL;

int main()
{
    signal(SIGINT, SIG_IGN);
    signal(SIGCHLD, childSignalHandler);

    Command::_currentCommand.prompt();

    return 0;
}
