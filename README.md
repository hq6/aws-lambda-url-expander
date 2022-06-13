# AWS Lambda URL Expander

A URL expander that runs in AWS Lambda and requires no special permissions
except internet access.

It follows all HTTP-based redirects to return the cannonical URL, so the user
can pass any URL without caring whether it is a shortened URL or not.

## Usage

1. Download the latest release zip from the [releases](https://github.com/hq6/aws-lambda-url-expander/releases) page.
2. Create an IAM Role to be used by your AWS Lambda.
```sh
# First create the trust-policy:
cat >> trust-policy.json <<EOF
{
 "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Principal": {
        "Service": ["lambda.amazonaws.com"]
      },
      "Action": "sts:AssumeRole"
    }
  ]
}
EOF

# Use the trust policy to create the role that AWS Lambda can assume
aws iam create-role \
--role-name url-expander-role \
--assume-role-policy-document file://trust-policy.json

# Grant the Role privileges to write to CloudWatch for logs
aws iam  attach-role-policy \
--role-name urlexpander-role \
--policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
```
3. Create the lambda function using the role and the code you just downloaded:
```sh
aws lambda create-function \
--function-name url-expander \
--role <INSERT_ROLE_ARN_FROM_ABOVE> \
--runtime provided \
--timeout 15 \
--memory-size 512 \
--handler url-expander \
--zip-file fileb://url-expander.zip
```
4. Invoke the lambda and examine the result. The `url` parameter is required,
   whie `max_time_ms` and `max_redirects` are optional. Note that `max_time_ms`
   is best-effort because of libcurl limitations.
```sh
request='{"url": "google.com", "max_time_ms": 1000, "max_redirects": 5}'
aws lambda invoke --function-name url-expander --payload "$(echo $request | base64)" output.txt
cat output.txt
```

NB: If you want to invoke your lambda from an AWS SDK, Amazon has [documentation](https://docs.aws.amazon.com/lambda/latest/dg/lambda-invocation.html) for that.

## Interface

Both the input and output are JSON fields with only top-level keys.


### Input keys
 * **url**: The initial url we want to expand / unshorten.
 * **max_time_ms**: The maximum amount of time we want curl to spend on making
   requests to expand the URL. This is best-effort, so callers should set it
   but still timeout their lambda invocations themselves. It is best-effort
   because even curl with libc-ares sometimes fails to respect the timeout for
   DNS queries.
 * **max_redirects**: The maximum number of redirects curl should follow. This
   should be set low enough to complete under the max_time_ms for most urls
   because curl can still retrieve the last url it followed when this is hit,
   while it cannot do so on a timeout.

### Output keys
 * **error_code**: Always present. This is set to 0 when the request finishes
   successfully. Hitting a redirect limit is considered success. In the case of
   failure, this is set to an integer that corresponds to a CURLcode.
 * **duration_ms**: The amount of time the execution spent executing curl_easy_perform.
 * **expanded_url**: Present iff error_code == 0. This is either the final URL
   or the last URL we found before hitting the redirect limit.
 * **reached_redirect_limit**: Present iff error_code == 0. True means that
   curl reached the redirect limit, so it is unknown whether expanded_url is
   the final URL in the redirect chain.
 * **error_message**: Present iff error_code != 0. This is the string
   description of the returned CURL error code.

## Limitations

Since this tool is based on libcurl, it only follows HTTP-based redirects. It
does not support JS-based redirects.

The timeout mechanism is not entirely reliable, due to limitations in libcurl's
DNS resolvers. Configuring libcurl to use libc-ares for DNS resolution is an
improvement over the default libcurl threaded resolver, but timeouts are still
only approximate. Empirically, setting a timeout of 300 ms results in most
requests finishing in 300 ms, but some fraction of requests timeout only after
500 ms and an even smaller fraction timeout after 1.2 seconds.

Callers should still set a target timeout to prevent lambda from running too
long and incurring unnecessary costs, with the understanding that it is
respected for most calls.
However, programmatic callers must not rely on the timeout to prevent
themselves from blocking. Instead, they should set their own timeout when
waiting for the Lambda completion.

## Developer Setup

See the [developer documentation](./HACKING.md).

## Appendix
### Why is URL expansion useful?

Scammers and phishers like to hide sketchy URLs behind URL
shorteners, making it harder to detect and block them. One of the most common
usages of URL expanders is to determine the real URL behind a shortened URL
so that one can determine whether the real URL is a dangerous destination.

### Why another URL expander?

Existing URL expanders / unshorteners that the author found suffer from one of
the following ailments:
1. They require too much configuration and effort to deploy.
2. They are a managed service, which means that if you are trying to use this
   in industry, you have to sign a contract and do a security review to make
   sure your data is safe.
3. They are poorly documented and complex.

Releases of this tool are intended to be easy to deploy and use with no
required configuration.

### Why run on AWS Lambda?

Many companies are running on AWS these days, and Lambda is actually one of the
simplest platforms to deploy onto. On Lambda, one can simply download a zip archive,
run 2 commands to create the lambda, and be able to invoke it using any AWS SDK.

By running this tool inside your own AWS account, there's no need for
infrastructure setup and no need to sign contracts if you are already an AWS
customer.

### Resources
 * https://aws.amazon.com/blogs/compute/introducing-the-c-lambda-runtime/
