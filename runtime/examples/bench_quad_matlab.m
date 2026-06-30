function bench_quad_matlab()
    f=@(x) exp(x).*sin(2*x);
    n=1001; x=linspace(0,2,n); y=f(x); dx=x(2)-x(1); R=200000;
    trapz(dx,y); t=tic; for r=1:R, v=trapz(dx,y); end; %#ok<NASGU>
    fprintf('MATLAB_trapz_n1001 %.1f ns/call\n', toc(t)/R*1e9);
end
