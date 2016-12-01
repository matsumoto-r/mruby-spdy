# SPDY Module for mruby  [![Build Status](https://travis-ci.org/matsumotory/mruby-spdy.png?branch=master)](https://travis-ci.org/matsumotory/mruby-spdy)
SPDY module for mruby using [spdylay](https://github.com/tatsuhiro-t/spdylay). You can access Web site using SPDY protocol from mruby applications or devices with mruby.

- [SPDY: An experimental protocol for a faster web](http://www.chromium.org/spdy/spdy-whitepaper) 

    As part of the "Let's make the web faster" initiative, we are experimenting with alternative protocols to help reduce the latency of web pages. One of these experiments is SPDY (pronounced "SPeeDY"), an application-layer protocol for transporting content over the web, designed specifically for minimal latency.

## TODO
This is a very early version, please test and report errors. Wellcome pull-request.
- replace uri parser to mruby-http
- implement some method (post...)
- implement some class (Server, Proxy...)
- write spdy callback function by Ruby block

## example
- SPDY by mruby

```ruby
r = SPDY::Client.get 'https://www.google.co.jp/'

r
r.body
r.body_length
r.spdy_version
r.stream_id
r.syn_reply
r.syn_stream
```

- response

```ruby
#r
{
    :syn_reply=>{
        "x-frame-options"=>"SAMEORIGIN", 
        "x-xss-protection"=>"1; mode=block", 
        "expires"=>"-1", 
        "p3p"=>"CP=\"This is not a P3P policy! See http://www.google.com/support/accounts/bin/answer.py?hl=en&answer=151657 for more info.\"", 
        "server"=>"gws", 
        "set-cookie"=>"; expires=Sun, 29-Jun-2014 17:16:55 GMT; path=/; domain=.google.co.jp; HttpOnly", 
        ":version"=>"HTTP/1.1", 
        "alternate-protocol"=>"443:quic", 
        "cache-control"=>"private, max-age=0", 
        "content-type"=>"text/html; charset=Shift_JIS", 
        "date"=>"Sat, 28 Dec 2013 17:16:55 GMT", 
        ":status"=>"200 OK"
    }, 
    :recieve_bytes=>953.0, 
    :body=>"<html> - (snip) - </html>", 
    :body_length=>953, 
    :spdy_proto_version=>4, 
    :stream_id=>1, 
    :syn_stream=>{
        ":method"=>"GET", 
        ":path"=>"/", 
        ":scheme"=>"https", 
        ":version"=>"HTTP/1.1", 
        ":host"=>"www.google.co.jp", 
        "accept"=>"*/*", 
        "user-agent"=>"mruby-spdy/0.0.1"
    }
}

#r.body
"<html> - (snip) - </html>"

#r.body_length
953

#r.spdy_version
4

#r.syn_stream
{
  ":method"=>"GET",
  ":path"=>"/",
  ":scheme"=>"https",
  ":version"=>"HTTP/1.1",
  ":host"=>"www.google.co.jp",
  "accept"=>"*/*",
  "user-agent"=>"mruby-spdy/0.0.1"
}

#r.syn_reply
{
  "x-frame-options"=>"SAMEORIGIN",
  "x-xss-protection"=>"1; mode=block",
  "expires"=>"-1",
  "p3p"=>"CP=\"This is not a P3P policy! See http://www.google.com/support/accounts/bin/answer.py?hl=en&answer=151657 for more info.\"",
  "server"=>"gws",
  "set-cookie"=>"; expires=Sun, 29-Jun-2014 17:16:55 GMT; path=/; domain=.google.co.jp; HttpOnly",
  ":version"=>"HTTP/1.1",
  "alternate-protocol"=>"443:quic",
  "cache-control"=>"private, max-age=0",
  "content-type"=>"text/html; charset=Shift_JIS",
  "date"=>"Sat, 28 Dec 2013 17:16:55 GMT",
  ":status"=>"200 OK"
}

```

## install by mrbgems
 - add conf.gem line to `build_config.rb`

```ruby
MRuby::Build.new do |conf|

  # ... (snip) ...

  conf.gem :github => 'matsumoto-r/mruby-spdy'
end
```

 - build

```
rake
```

# License
under the MIT License:

* http://www.opensource.org/licenses/mit-license.php


