// Same browser setup as tests/frontend.cjs. Uses local mock endpoints only.
const { chromium } = require('playwright');
const http = require('http'), fs = require('fs'), path = require('path'), assert = require('assert/strict');
const html = fs.readFileSync(path.join(__dirname,'../AIApps/ChatServer/resource/entry.html'));
let registrations = 0, logins = 0, registrationOpen = true;
const server = http.createServer(async (req,res) => {
  let raw = ''; for await (const part of req) raw += part;
  const reply = (status, body) => {res.statusCode=status;res.setHeader('Content-Type','application/json');res.end(JSON.stringify(body));};
  if (req.url === '/api/config') return reply(200,{registrationOpen});
  if (req.url === '/register') {++registrations;return reply(200,{success:true,userId:1});}
  if (req.url === '/login') {
    ++logins;
    return logins === 1 ? reply(429,{message:'操作太频繁，请稍后重试'}) : reply(200,{success:true,userId:1});
  }
  res.setHeader('Content-Type','text/html;charset=utf-8');res.end(req.url==='/chat' ? '<h1>聊天</h1>' : html);
});
(async()=>{
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const browser=await chromium.launch({headless:true,...(process.env.CHROME_PATH ? {executablePath:process.env.CHROME_PATH} : {})});
  try {
    const page=await browser.newPage(); const errors=[];page.on('pageerror',e=>errors.push(e.message));
    const origin='http://127.0.0.1:'+server.address().port;
    await page.goto(origin);
    await page.getByText('去注册账号',{exact:true}).click();
    await page.locator('#register-username').fill('new_user');
    await page.locator('#register-password').fill('short');
    await page.getByRole('button',{name:'注册',exact:true}).click();
    assert.equal(registrations,0);
    await page.locator('#register-password').fill('a long password for this account');
    await page.getByRole('button',{name:'注册',exact:true}).click();
    await page.getByText('注册成功，请使用新账号登录',{exact:true}).waitFor();
    assert.equal(registrations,1);
    assert.equal(await page.locator('#login-password').inputValue(),'');
    assert.equal(await page.locator('#register-password').inputValue(),'');
    await page.locator('#login-password').fill('a long password for this account');
    await page.getByRole('button',{name:'登录',exact:true}).click();
    await page.getByText('操作太频繁，请稍后重试',{exact:true}).waitFor();
    assert.equal(await page.getByRole('button',{name:'登录',exact:true}).isEnabled(),true);
    await page.getByRole('button',{name:'登录',exact:true}).click();
    await page.waitForURL(origin+'/chat');
    registrationOpen=false;
    await page.goto(origin);
    await page.getByText('新用户注册暂时关闭',{exact:true}).waitFor();
    assert.equal(await page.locator('#register-link').getAttribute('onclick'),null);
    assert.deepEqual(errors,[]);
    console.log('accounts UI: password validation, registration, credential clearing, rate-limit message, login and closed registrations passed');
  } finally {await browser.close();await new Promise(resolve=>server.close(resolve));}
})().catch(error=>{console.error(error);process.exitCode=1;server.close();});
